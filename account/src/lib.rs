//! Linux account state for the upstream UU client. Signing material is supplied at runtime.
//! The APK fingerprint comes from upstream scripts/extract_key.py, GPL-3.0-or-later.
use std::env;
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{self, ErrorKind, Read, Seek, Write};
use std::os::fd::AsRawFd;
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use sha2::{Digest, Sha256};
use zip::ZipArchive;

const KEY_LEN: usize = 24;
const KEY_SHA256: [u8; 32] = [
    0x40, 0xcb, 0x3a, 0x32, 0xf9, 0xf6, 0xf9, 0x0d, 0x99, 0x62, 0xfb, 0x97, 0x36, 0x68, 0x5b, 0xb8,
    0x81, 0x97, 0x9e, 0x84, 0x8b, 0x59, 0x56, 0x64, 0x9b, 0x6d, 0xae, 0x3c, 0xc9, 0x57, 0x52, 0x8c,
];
const MAX_DEX: usize = 128 * 1024 * 1024;
const MAX_APK: u64 = 128 * 1024 * 1024;
// Linked from the Android TV entry at https://uuyc.163.com/download/ via adl.netease.com.
const OFFICIAL_APK: &str = "https://api.nrd.nie.163.com/api/v1/release/dl/5?channel=TV_gwqd";

fn invalid(detail: &'static str) -> io::Error {
    io::Error::new(ErrorKind::InvalidData, detail)
}

fn state_dir() -> io::Result<PathBuf> {
    let absolute_env = |name| {
        env::var_os(name)
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
    };
    let base = absolute_env("XDG_STATE_HOME")
        .or_else(|| absolute_env("HOME").map(|home| home.join(".local/state")))
        .ok_or_else(|| invalid("需要绝对路径的 HOME 或 XDG_STATE_HOME"))?;
    Ok(base.join("youyou-linux"))
}

fn lock_directory(dir: &Path) -> io::Result<File> {
    DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
    let meta = fs::symlink_metadata(dir)?;
    if !meta.is_dir() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o077 != 0 {
        return Err(io::Error::new(
            ErrorKind::PermissionDenied,
            "账号目录必须由当前用户拥有，且仅限该用户访问（权限 0700）",
        ));
    }
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(dir.join("client.lock"))?;
    // Losing the process automatically releases the lock, including after a crash.
    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } < 0 {
        let err = io::Error::last_os_error();
        return if err.kind() == ErrorKind::WouldBlock {
            Err(io::Error::new(
                ErrorKind::WouldBlock,
                "另一个 YOUYOU 窗口正在使用当前账号目录",
            ))
        } else {
            Err(err)
        };
    }
    Ok(file)
}

fn private_read(path: &Path, max: usize) -> io::Result<Vec<u8>> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)?;
    let meta = file.metadata()?;
    if !meta.is_file() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o077 != 0 {
        return Err(io::Error::new(
            ErrorKind::PermissionDenied,
            "本地凭据必须是当前用户的私有普通文件（权限 0600）",
        ));
    }
    bounded_read(file, max)
}

fn bounded_read(reader: impl Read, max: usize) -> io::Result<Vec<u8>> {
    let mut out = Vec::new();
    reader.take(max as u64 + 1).read_to_end(&mut out)?;
    if out.len() > max {
        return Err(invalid("文件超过大小限制"));
    }
    Ok(out)
}

// Caller holds the directory lock. A failed import never replaces an existing good key.
fn commit(dir: &Path, name: &str, bytes: &[u8]) -> io::Result<()> {
    let tmp = dir.join(format!(".{name}.new"));
    match fs::remove_file(&tmp) {
        Ok(()) => (),
        Err(e) if e.kind() == ErrorKind::NotFound => (),
        Err(e) => return Err(e),
    }
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&tmp)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(tmp, dir.join(name))?;
    File::open(dir)?.sync_all()
}

fn uuid() -> io::Result<String> {
    let mut bytes = [0u8; 16];
    File::open("/dev/urandom")?.read_exact(&mut bytes)?;
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    let mut id = String::with_capacity(36);
    for (i, byte) in bytes.iter().enumerate() {
        if [4, 6, 8, 10].contains(&i) {
            id.push('-');
        }
        use std::fmt::Write as _;
        write!(&mut id, "{byte:02x}").expect("writing to String");
    }
    Ok(id)
}

fn client_id(dir: &Path) -> io::Result<String> {
    match private_read(&dir.join("client-id"), 36) {
        Ok(bytes)
            if bytes.len() == 36
                && bytes.iter().enumerate().all(|(i, b)| {
                    if [8, 13, 18, 23].contains(&i) {
                        *b == b'-'
                    } else {
                        b.is_ascii_hexdigit()
                    }
                }) =>
        {
            Ok(String::from_utf8(bytes).expect("ASCII validated"))
        }
        Ok(_) => Err(invalid("本地 client-id 损坏，请先备份账号目录")),
        Err(e) if e.kind() == ErrorKind::NotFound => {
            let id = uuid()?;
            commit(dir, "client-id", id.as_bytes())?;
            Ok(id)
        }
        Err(e) => Err(e),
    }
}

/// Keep this handle alive while the native client accesses its per-user working directory.
pub struct Account {
    state_dir: PathBuf,
    key: Vec<u8>,
    client_id: String,
    _lock: File,
}

impl Account {
    pub fn state_dir(&self) -> &Path {
        &self.state_dir
    }
    pub fn key(&self) -> &[u8] {
        &self.key
    }
    pub fn client_id(&self) -> &str {
        &self.client_id
    }
}

pub fn open() -> io::Result<Account> {
    let state_dir = state_dir()?;
    let lock = lock_directory(&state_dir)?;
    let key = private_read(&state_dir.join("signing.key"), KEY_LEN).map_err(|e| {
        if e.kind() == ErrorKind::NotFound {
            io::Error::new(
                ErrorKind::NotFound,
                "请先导入官方 UU 远程电视版 APK 或匹配的 signing.key",
            )
        } else {
            e
        }
    })?;
    verify(&key)?;
    let client_id = client_id(&state_dir)?;
    Ok(Account {
        state_dir,
        key,
        client_id,
        _lock: lock,
    })
}

fn verify(key: &[u8]) -> io::Result<()> {
    if key.len() != KEY_LEN || Sha256::digest(key).as_slice() != KEY_SHA256 {
        Err(invalid("签名材料与上游验证的 UU 远程电视版 3.5.0 不匹配"))
    } else {
        Ok(())
    }
}

fn install(key: &[u8]) -> io::Result<()> {
    verify(key)?;
    let dir = state_dir()?;
    let _lock = lock_directory(&dir)?;
    commit(&dir, "signing.key", key)
}

pub fn import_key(path: &Path) -> io::Result<()> {
    install(&bounded_read(File::open(path)?, KEY_LEN)?)
}

pub fn import_apk(path: &Path) -> io::Result<()> {
    install(&apk_key(File::open(path)?)?)
}

/// User-initiated setup. Never executes or keeps the APK. curl handles the Linux CA store.
pub fn download_official() -> io::Result<()> {
    let dir = state_dir()?;
    let _lock = lock_directory(&dir)?;
    let tmp = dir.join(format!(".tv-{}.apk", uuid()?));
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&tmp)?;
    // The open descriptor stays valid; even an interrupted process leaves no archive on disk.
    fs::remove_file(tmp)?;
    let mut curl = Command::new("/usr/bin/curl")
        .args([
            "--disable",
            "--silent",
            "--fail",
            "--location",
            "--max-redirs",
            "5",
            "--proto",
            "=https",
            "--proto-redir",
            "=https",
            "--connect-timeout",
            "12",
            "--max-time",
            "120",
            "--max-filesize",
            &MAX_APK.to_string(),
            "--output",
            "-",
            "--",
            OFFICIAL_APK,
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| {
            io::Error::new(
                e.kind(),
                format!("无法启动 /usr/bin/curl，请安装 curl 或手动导入 APK（{e}）"),
            )
        })?;
    let pipe = curl
        .stdout
        .take()
        .expect("configured curl stdout as a pipe");
    let copied = io::copy(&mut pipe.take(MAX_APK + 1), &mut file);
    if copied.is_err() || copied.as_ref().is_ok_and(|&n| n > MAX_APK) {
        let _ = curl.kill();
        let _ = curl.wait();
        copied?;
        return Err(invalid("官网下载超过大小限制"));
    }
    let status = curl.wait()?;
    if !status.success() {
        return Err(io::Error::other(format!(
            "官网下载失败（curl {status}），可重试或从本地导入电视版 APK"
        )));
    }
    file.rewind()?;
    let key = apk_key(file)?;
    commit(&dir, "signing.key", &key)
}

fn apk_key(file: File) -> io::Result<Vec<u8>> {
    let meta = file.metadata()?;
    if !meta.is_file() || meta.len() > MAX_APK {
        return Err(invalid("APK 文件无效或超过 128 MiB"));
    }
    let mut archive = ZipArchive::new(file).map_err(|_| invalid("文件不是有效 APK / ZIP"))?;
    let dex = archive
        .by_name("classes.dex")
        .map_err(|_| invalid("APK 中缺少 classes.dex"))?;
    if dex.size() > MAX_DEX as u64 {
        return Err(invalid("classes.dex 超过大小限制"));
    }
    let dex = bounded_read(dex, MAX_DEX)?;
    let key = dex_key(&dex, &KEY_SHA256)?;
    Ok(key.to_vec())
}

fn dex_key<'a>(dex: &'a [u8], expected: &[u8; 32]) -> io::Result<&'a [u8]> {
    if dex.len() < 112 || dex.get(..4) != Some(b"dex\n") || dex[7] != 0 {
        return Err(invalid("DEX 文件头无效"));
    }
    let u32_at = |i| u32::from_le_bytes(dex[i..i + 4].try_into().expect("bounded header")) as usize;
    let count = u32_at(56);
    let offset = u32_at(60);
    let table_size = count
        .checked_mul(4)
        .ok_or_else(|| invalid("DEX 字符串表溢出"))?;
    let table = dex
        .get(offset..)
        .and_then(|d| d.get(..table_size))
        .ok_or_else(|| invalid("DEX 字符串表越界"))?;
    for entry in table.chunks_exact(4) {
        let pos = u32::from_le_bytes(entry.try_into().expect("four-byte entry")) as usize;
        let data = dex
            .get(pos..)
            .ok_or_else(|| invalid("DEX 字符串偏移越界"))?;
        let prefix_end = data
            .iter()
            .take(5)
            .position(|b| b & 0x80 == 0)
            .ok_or_else(|| invalid("DEX 字符串长度字段无效"))?;
        let start = prefix_end + 1;
        if data.get(start + KEY_LEN) != Some(&0) {
            continue;
        }
        let bytes = &data[start..start + KEY_LEN];
        if !bytes.contains(&0) && Sha256::digest(bytes).as_slice() == expected {
            return Ok(bytes);
        }
    }
    Err(invalid(
        "APK 中未找到匹配的签名材料，请选择兼容的 UU 远程电视版 APK（上游参考版本 3.5.0）",
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::{symlink, PermissionsExt};

    #[test]
    fn dex_import_checks_bounds_fingerprint_and_terminator() {
        let fixture = b"synthetic-fixture-123456";
        assert_eq!(fixture.len(), KEY_LEN);
        let mut dex = vec![0; 116];
        dex[..8].copy_from_slice(b"dex\n035\0");
        dex[56..60].copy_from_slice(&1u32.to_le_bytes());
        dex[60..64].copy_from_slice(&112u32.to_le_bytes());
        dex[112..116].copy_from_slice(&116u32.to_le_bytes());
        dex.push(KEY_LEN as u8);
        dex.extend_from_slice(fixture);
        dex.push(0);
        let hash = Sha256::digest(fixture).into();
        assert_eq!(dex_key(&dex, &hash).unwrap(), fixture);
        assert!(dex_key(&dex, &KEY_SHA256).is_err());
        dex.pop();
        assert!(dex_key(&dex, &hash).is_err());
        dex[112..116].fill(0xff);
        assert!(dex_key(&dex, &hash).is_err());
        dex[56..60].fill(0xff);
        assert!(dex_key(&dex, &hash).is_err());
    }

    #[test]
    fn state_lock_serializes_writes_and_identity_survives_restart() {
        let path = env::temp_dir().join(format!("youyou-test-{}", uuid().unwrap()));
        let lock = lock_directory(&path).unwrap();
        assert_eq!(
            lock_directory(&path).unwrap_err().kind(),
            ErrorKind::WouldBlock
        );
        assert_eq!(fs::metadata(&path).unwrap().mode() & 0o777, 0o700);
        let id = client_id(&path).unwrap();
        assert_eq!(id.len(), 36);
        assert_eq!(id.as_bytes()[14], b'4');
        assert_eq!(
            fs::metadata(path.join("client-id")).unwrap().mode() & 0o777,
            0o600
        );
        drop(lock);
        let _lock = lock_directory(&path).unwrap();
        assert_eq!(id, client_id(&path).unwrap());
        symlink(path.join("client-id"), path.join("signing.key")).unwrap();
        assert!(private_read(&path.join("signing.key"), 36).is_err());
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        assert_eq!(
            lock_directory(&path).unwrap_err().kind(),
            ErrorKind::PermissionDenied
        );
        fs::remove_dir_all(path).unwrap();
    }
}
