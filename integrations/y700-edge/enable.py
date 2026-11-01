#!/usr/bin/env python3
"""Enable or restore Y700's per-user KWin side-edge integration."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

PLUGIN = "y700-edge"
# Stock legion-y700-gen5 and the tablet build with the Show Desktop shortcut.
STOCK = {
    "4a8aabbde071072080e115fce9bc94e2c75aa0290d76c5f48ffbb10be535d930",
    "d1212fc465b47b81dcee0d3a022d3abb86ba55a5fd2236719727ee37ab1b4370",
}
BLOCKED = b"function blocked(added, removed) {\n"
GUARD = b"    if (youyouOwnsSideEdges(removed)) return true;\n"
START = b"\nupdate(null, null);\n"


def content(original, hooks, restore=False):
    injected = b"\n" + hooks.rstrip() + b"\n"
    base = original
    if original.count(injected + START) == 1 and original.count(BLOCKED + GUARD) == 1:
        base = original.replace(injected + START, START, 1).replace(BLOCKED + GUARD, BLOCKED, 1)
        if not restore:
            # A maintenance release can keep these hooks alongside new features.
            return original
    if hashlib.sha256(base).hexdigest() not in STOCK:
        raise ValueError("未识别此版本的 y700-edge，已保留原文件。")
    if restore:
        return base
    return base.replace(BLOCKED, BLOCKED + GUARD, 1).replace(START, injected + START, 1)


def kwin(method, *args):
    raw = subprocess.check_output(["busctl", "--user", "--timeout=6s", "--json=short", "call",
                                   "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting",
                                   method, *args], timeout=8, text=True)
    data = json.loads(raw)["data"] if raw.strip() else []
    return data[0] if data else None


def loaded():
    return kwin("isScriptLoaded", "s", PLUGIN)


def reload_plugin():
    if loaded():
        if not kwin("unloadScript", "s", PLUGIN):
            raise RuntimeError("KWin 未卸载 y700-edge。")
        for _ in range(30):  # unloadScript schedules deletion on KWin's event loop.
            if not loaded():
                break
            time.sleep(0.05)
        else:
            raise RuntimeError("KWin 仍在使用旧脚本。")
    kwin("start")  # Respects the enabled plugins; already running scripts stay loaded.
    # Loading JavaScript is asynchronous. KWin removes scripts whose evaluation failed.
    time.sleep(0.5)
    if not loaded():
        raise RuntimeError("KWin 未载入 y700-edge。")


def replace_script(path, old, new):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as f:
        temp = Path(f.name)
        try:
            f.write(new)
            f.flush()
            os.fchmod(f.fileno(), path.stat().st_mode & 0o777)
            os.fsync(f.fileno())
            if path.read_bytes() != old:
                raise RuntimeError("更新期间脚本发生变化，已停止。")
            os.replace(temp, path)
        finally:
            temp.unlink(missing_ok=True)


def install(path, hooks, restore=False):
    original = path.read_bytes()
    updated = content(original, hooks, restore)
    if not loaded():
        raise RuntimeError("当前 KDE 会话没有启用 y700-edge。")
    if updated == original:
        reload_plugin()
        return
    replace_script(path, original, updated)
    try:
        reload_plugin()
    except (OSError, RuntimeError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        replace_script(path, updated, original)
        try:
            reload_plugin()
        except (OSError, RuntimeError, subprocess.SubprocessError) as rollback_error:
            raise RuntimeError(f"已还原原始文件，但 KWin 重载失败：{rollback_error}") from error
        raise RuntimeError("更新未生效，已还原原始文件并重载原插件。") from error


def main():
    parser = argparse.ArgumentParser(description="让 Y700 的 KWin 侧边返回在串流期间让出触屏。")
    parser.add_argument("--restore", action="store_true", help="还原原版 KWin 脚本")
    args = parser.parse_args()
    if os.geteuid() == 0:
        raise RuntimeError("请在平板的 KDE 会话中以普通用户运行。")
    root = Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share").resolve()
    path = root / "kwin/scripts" / PLUGIN / "contents/code/main.js"
    if not path.is_file() or path.resolve() != path or path.stat().st_uid != os.getuid():
        raise RuntimeError("未找到当前用户安装的 y700-edge KWin 脚本。")
    hook = Path(__file__).resolve().with_name("stream-edges.js")
    if not hook.is_file():
        hook = Path("/usr/share/youyou-linux/y700-edge/stream-edges.js")
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        install(path, hook.read_bytes(), args.restore)
    finally:
        os.close(fd)
    print("已还原 Y700 侧边返回。" if args.restore else "Y700 串流侧滑已启用。")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        sys.exit(str(exc))
