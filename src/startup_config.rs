//! Read the renderer preference once, before SDL is initialized.
use std::{fs::File, io::Read};

const MAX_CONFIG_BYTES: usize = 1024 * 1024;

fn configured_gpu_video(bytes: &[u8]) -> bool {
    if bytes.len() > MAX_CONFIG_BYTES {
        return false;
    }
    let Ok(text) = std::str::from_utf8(bytes) else {
        return false;
    };
    // Match native ConfigStore's flat key/value format, ASCII whitespace,
    // trailing # comments and first-wins duplicate handling.
    fn trim(value: &str) -> &str {
        value.trim_matches([' ', '\t', '\r', '\n'])
    }
    for line in text.lines() {
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        if trim(key) == "gpu_video" {
            return trim(value.split('#').next().unwrap_or_default()) == "1";
        }
    }
    false
}

pub fn gpu_video_configured() -> bool {
    let Ok(directory) = youyou_account::state_dir() else {
        return false;
    };
    let path = directory.join("config.ini");
    let Ok(metadata) = path.metadata() else {
        return false;
    };
    if !metadata.is_file() || metadata.len() > MAX_CONFIG_BYTES as u64 {
        return false;
    }
    let Ok(file) = File::open(path) else {
        return false;
    };
    let mut bytes = Vec::new();
    if file
        .take(MAX_CONFIG_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .is_err()
    {
        return false;
    }
    configured_gpu_video(&bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn accepts_native_whitespace_and_comments() {
        assert!(configured_gpu_video(
            b"# gpu_video=0\r\n\nquality=3\n \tgpu_video \t= 1 \t# enabled\r\n"
        ));
        assert!(!configured_gpu_video(b"# gpu_video=1\n;gpu_video=1\n"));
        assert!(!configured_gpu_video(b"GPU_VIDEO=1\n"));
    }

    #[test]
    fn first_key_wins_including_disabled_or_invalid_values() {
        assert!(configured_gpu_video(b"gpu_video=1\ngpu_video=0\n"));
        for first in ["0", "", "true", "2", "1junk", "+1", "1;comment"] {
            assert!(!configured_gpu_video(
                format!("gpu_video={first}\ngpu_video=1\n").as_bytes()
            ));
        }
        assert!(!configured_gpu_video(b""));
    }

    #[test]
    fn rejects_oversized_or_invalid_utf8_even_after_enabled_key() {
        let mut bytes = b"gpu_video=1\n#".to_vec();
        bytes.resize(MAX_CONFIG_BYTES, b'x');
        assert!(configured_gpu_video(&bytes));
        bytes.push(b'x');
        assert!(!configured_gpu_video(&bytes));
        assert!(!configured_gpu_video(b"gpu_video=1\n#\xff"));
    }
}
