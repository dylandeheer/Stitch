"""Arm one owner launch of the known Stitch 10 image, or restore the exact launcher."""
import argparse
import fcntl
import hashlib
import json
import os
import stat
import tempfile
from pathlib import Path

BASE = Path.home() / "Applications/Lichii/oot-amd-candidate-20261107"
SOURCE = "da4c8bd084e656ca67b91355ef1d6f53b1afd677"
IMAGE = "Stitch-OoT-AMD-10-x86_64.AppImage"
IMAGE_SHA = "3807b399056cf5b17fb2173160152578b3c3d48ff165330c51a0192feac02128"
CLEAN_SHA = "4c708812b1efe4c1ed26d6b2f50543613ffa95dc40a917ae32040db688c73121"
PRIVATE = BASE / "diagnostics/private-perf/stitch10-named"
MARKER = BASE / "stitch10-named-jit-once"
BLOCK = b'''if rmdir -- "$base/stitch10-named-jit-once" 2>/dev/null; then
    umask 077
    unset STITCH_ORDERED_FIFO_MACRO STITCH_ORDERED_FIFO_MODE_FILE
    export PERF_BUILDID_DIR="$base/diagnostics/private-perf/stitch10-named"
fi
'''
ANCHOR = b'exec >>"$base/diagnostics/launcher.log" 2>&1\n'


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic_write(path, data, mode):
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        name = Path(output.name)
        try:
            output.write(data)
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
            os.replace(name, path)
        finally:
            name.unlink(missing_ok=True)


def operate(action):
    os.umask(0o077)
    launcher = BASE / "launch-lichii.sh"
    backup = PRIVATE / "clean-launcher-backup.sh"
    for path in (BASE, BASE / "diagnostics", launcher, BASE / IMAGE, BASE / "stitch-10.json"):
        require(path == path.resolve(strict=True), f"Unexpected symlink: {path}")
    require(json.loads((BASE / "stitch-10.json").read_text())["source_revision"] == SOURCE, "Installation mismatch")
    require(digest((BASE / IMAGE).read_bytes()) == IMAGE_SHA, "Image digest mismatch")
    for path in [PRIVATE.parent, PRIVATE]:
        path.mkdir(mode=0o700, exist_ok=True)
        st = path.lstat()
        require(stat.S_ISDIR(st.st_mode) and stat.S_IMODE(st.st_mode) == 0o700 and st.st_uid == os.getuid(),
                "Expected a private, owner-owned diagnostics directory")
    with (PRIVATE / "setup.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current = launcher.read_bytes()
        clean = current if digest(current) == CLEAN_SHA else backup.read_bytes()
        require(digest(clean) == CLEAN_SHA and clean.count(ANCHOR) == 1, "Unexpected launcher or backup")
        named = clean.replace(ANCHOR, BLOCK + ANCHOR)
        require(current in (clean, named), "Current launcher differs from both known revisions")
        if os.path.lexists(MARKER):
            require(stat.S_ISDIR(MARKER.lstat().st_mode) and not any(MARKER.iterdir()), "Invalid one-time marker")
        if action == "arm":
            require(not os.path.lexists(BASE / "stitch10-verify-session"), "Verification session armed")
            if backup.exists():
                require(backup.read_bytes() == clean, "Unexpected backup")
            else:
                atomic_write(backup, clean, 0o700)
            if current != named:
                atomic_write(launcher, named, stat.S_IMODE(launcher.stat().st_mode))
            MARKER.mkdir(mode=0o700, exist_ok=True)
        else:
            if MARKER.exists():
                MARKER.rmdir()
            if current != clean:
                atomic_write(launcher, clean, stat.S_IMODE(launcher.stat().st_mode))
        expected = named if action == "arm" else clean
        require(launcher.read_bytes() == expected, "Launcher readback failed")
        print(json.dumps({"action": action, "source": SOURCE, "image_sha256": IMAGE_SHA,
                          "launcher_sha256": digest(expected), "marker_armed": MARKER.is_dir(),
                          "starts_or_stops_emulator": False}))


if __name__ == "__main__":
    args = argparse.ArgumentParser()
    args.add_argument("action", choices=("arm", "restore"))
    operate(args.parse_args().action)
