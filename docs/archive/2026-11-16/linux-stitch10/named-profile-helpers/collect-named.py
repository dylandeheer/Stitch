"""Run on the owner host. Private perf data/maps stay inside the named capture directory."""
import bisect
import collections
import contextlib
import ctypes
import hashlib
import json
import os
import re
import shutil
import socket
import stat
import sys
import subprocess
import tempfile
from pathlib import Path

BASE = Path.home() / "Applications/Lichii/oot-amd-candidate-20261107"
PRIVATE = BASE / "diagnostics/private-perf/stitch10-named"
SOURCE = "da4c8bd084e656ca67b91355ef1d6f53b1afd677"
EXE_SHA = "0da9286a7368f0b1ae95d4165ea809fd2263a1d627d6f4d9849fbeb9df471789"
TITLE = "0400486024DEC000"
BLOCK = re.compile(r"a64_([a-fA-F0-9]{16})_fpcr[a-fA-F0-9]{8}")
SAMPLE = re.compile(r"\s*(\S(?:.*?\S)?)\s+(\d+)\s+(\d+)\s+([0-9a-fA-F]+)\s*$")
# Fixed locations are only probes. Attribution requires the complete page hash before and after.
PAGES = [
    (0x80672000, "d56f37aaaec598fb7b843f15c5d56505996efbae8cfbcaa833a0cdbf82f9361e",
     0x370, 0x7dc, "Zstandard-compatible sequence decode/expand"),
    (0x80b11000, "02d9c2dc30a85d5c0942117f1898be88dc94ed8e74ba5468d9b48e1b24a15414",
     0xc64, 0xda0, "bounded string comparison in binary search A"),
    (0x80b13000, "66d9cee3f0d52d76b6b2484de7a1eeafdbd831984015c91d6e51eec676fa38b2",
     0xe24, 0xf60, "bounded string comparison in binary search B"),
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def rpc(method, params):
    with socket.create_connection(("127.0.0.1", 6545), timeout=4) as sock:
        sock.sendall(json.dumps({"id": 1, "method": method, "params": params}).encode() + b"\n")
        result = json.loads(sock.makefile("rb").readline())
        if "result" not in result:
            raise ValueError(f"{method} unavailable")
        return result["result"]


def identity(proc):
    fields = (proc / "stat").read_text().rsplit(") ", 1)[1].split()
    exe = (proc / "exe").stat()
    return fields[19], exe.st_dev, exe.st_ino


def check_game():
    state = rpc("status", {"fps_window_ms": 0})
    require(state["program_id"] == TITLE and state["scm_rev"] == SOURCE and state["powered_on"]
            and not state["paused"] and state["renderer"] == "vulkan" and not state["rejected"],
            "Expected unpaused, original Vulkan title on installed Stitch 10")
    return state["presents"]


def match_pages():
    result = []
    for page, digest, *_ in PAGES:
        try:
            data = rpc("read_memory", {"address": hex(page), "size": 4096})
            raw = bytes.fromhex(data["hex"])
            ok = len(raw) == 4096 and hashlib.sha256(raw).hexdigest() == digest
        except (ValueError, OSError):
            ok = False
        result.append(ok)
    return result


@contextlib.contextmanager
def watch_generation(path):
    libc = ctypes.CDLL(None, use_errno=True)
    fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
    require(fd >= 0, "inotify unavailable")
    try:
        # Dynarmic closes the writer before EVERY clear, including truncate on the same inode.
        events = 0x00000008 | 0x00000400 | 0x00000800  # CLOSE_WRITE, DELETE_SELF, MOVE_SELF
        require(libc.inotify_add_watch(fd, os.fsencode(path), events) >= 0, "Cannot watch map")
        yield
        try:
            change = os.read(fd, 64 * 1024)
        except BlockingIOError:
            change = b""
        require(not change, "Perf map generation changed or watcher overflowed; discard capture")
    finally:
        os.close(fd)


def freeze_prefix(path, output, end=None):
    with path.open("rb") as src, output.open("xb") as dst:
        n = os.fstat(src.fileno()).st_size if end is None else end
        left = n
        while left:
            chunk = src.read(min(1 << 20, left))
            require(chunk, "Map was truncated")
            dst.write(chunk)
            left -= len(chunk)
    require(n > 0, "Empty JIT map")
    with output.open("rb+") as f:
        f.seek(-1, os.SEEK_END)
        if f.read(1) != b"\n":
            # An fwrite may be underway. Discard the unfinished record, never resolve it.
            f.seek(max(0, n - 4096))
            tail = f.read()
            pos = tail.rfind(b"\n")
            require(pos >= 0, "Unterminated map record")
            f.truncate(n - len(tail) + pos + 1)


def matching_ranges(path, ips, prefix_size):
    found = {}
    ambiguous = set()
    prefix_sha = hashlib.sha256()
    prefix_rows = 0
    with path.open("rb") as f:
        size = 0
        for line in f:
            start, length, name = line.rstrip(b"\n").split(None, 2)
            address = int(start, 16)
            span = int(length, 16)
            size += len(line)
            old = size <= prefix_size
            if old:
                prefix_sha.update(line)
                prefix_rows += 1
            lo = bisect.bisect_left(ips, address)
            hi = bisect.bisect_left(ips, address + span, lo)
            for ip in ips[lo:hi]:
                label = name.decode("ascii")
                if not old or (ip in found and found[ip] != label):
                    # New or reused sampled ranges cannot be timestamped inside perf.map.
                    ambiguous.add(ip)
                else:
                    found[ip] = label
    return found, ambiguous, prefix_sha.hexdigest(), prefix_rows


def read_samples(file):
    rows = []
    for line in file.read_text().splitlines():
        m = SAMPLE.fullmatch(line)
        require(m is not None, "Unparsed perf sample")
        thread, tid, weight, ip = m.groups()
        rows.append((thread, int(tid), int(weight), int(ip, 16)))
    return rows


def report(data, rows, names, ambiguous, known):
    weighted = collections.Counter()
    samples = collections.Counter()
    blocks = collections.Counter()
    threads = collections.Counter()
    for comm, tid, weight, ip in rows:
        matched = names.get(ip, "")
        b = BLOCK.fullmatch(matched)
        if ip in ambiguous:
            category = "map added or overlapping during capture"
        elif b:
            pc = int(b[1], 16)
            blocks[pc] += weight
            category = "other mapped guest block"
            for ok, (page, _, start, end, desc) in zip(known, PAGES):
                if ok and page + start <= pc < page + end:
                    category = desc
                    break
        elif matched:
            category = "mapped Dynarmic helper"
        else:
            category = "outside matched JIT ranges (includes host native)"
        weighted[category] += weight
        samples[category] += 1
        threads[comm] += weight
    for ok, (*_, desc) in zip(known, PAGES):
        if ok:
            weighted[desc] += 0
    total = sum(weighted.values())
    require(total > 0, "Empty sample weights")
    require({f"CPUCore_{i}" for i in range(6)} <= set(threads), "Missing guest core(s) in samples")
    require(bool(blocks), "No sampled A64 JIT block resolved")
    data["estimated_cycles"] = total
    data["rows"] = [
        {"category": label, "samples": samples[label], "estimated_cycles": w, "pct_process": 100 * w / total}
        for label, w in weighted.most_common()
    ]
    data["matched_historical_pages"] = {p[-1]: ok for p, ok in zip(PAGES, known)}
    data["guest_threads_pct_process"] = {t: 100 * w / total for t, w in threads.items() if re.fullmatch("CPUCore_[0-5]", t)}
    return [{"guest_pc": hex(pc), "weight": w, "pct_process": 100*w/total} for pc, w in blocks.most_common()]


def run():
    os.umask(0o077)
    require(PRIVATE.resolve(strict=True) == PRIVATE and stat.S_IMODE(PRIVATE.stat().st_mode) == 0o700,
            "Private launch directory mismatch")
    matches = []
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            if (proc / "comm").read_text().strip() == "stitch-bin":
                matches.append(proc)
        except (FileNotFoundError, ProcessLookupError):
            pass
    require(len(matches) == 1, "Expected exactly one owner-launched Stitch process")
    proc = matches[0]
    ident = identity(proc)
    require(hashlib.sha256((proc / "exe").read_bytes()).hexdigest() == EXE_SHA, "Loaded executable mismatch")
    env = dict(x.split("=", 1) for x in (proc / "environ").read_text().split("\0") if "=" in x)
    require(env.get("PERF_BUILDID_DIR") == str(PRIVATE) and env.get("STITCH_OOT_COMPARE_LINK") == "1"
            and env.get("APPIMAGE") == str(BASE / "Stitch-OoT-AMD-10-x86_64.AppImage")
            and env.get("EDEN_NX2_FORCE_MESH_FALLBACK") == "1"
            and env.get("XDG_CONFIG_HOME") == str(BASE / "profile10/config")
            and not (BASE / "stitch10-named-jit-once").exists(), "Profile launch mismatch")
    for flag in ("EDEN_NX2_GPU_PROFILE", "EDEN_DKB_FRAME_TRACE", "EDEN_NX2_FAST_DRAW_VERIFY",
                 "EDEN_NX2_TEXTURE_MEMO_VERIFY", "EDEN_NX2_UPLOAD_TIMING", "STITCH_JIT_WARMUP",
                 "MESA_VK_TRACE", "MESA_VK_TRACE_TRIGGER", "RADV_THREAD_TRACE", "STITCH_SPLIT_WAIT_TRACE"):
        require(not env.get(flag), "Incompatible diagnostic: " + flag)
    require(not env.get("STITCH_ORDERED_FIFO_MACRO") and not env.get("STITCH_ORDERED_FIFO_MODE_FILE"), "FIFO verification must be off in a named diagnostic run")
    check_game()
    names = set()
    for name_file in (proc / "task").glob("*/comm"):
        try:
            names.add(name_file.read_text().strip())
        except (FileNotFoundError, ProcessLookupError):
            pass
    require({f"CPUCore_{i}" for i in range(6)} <= names, "Expected six guest threads")
    perf = shutil.which("perf")
    require(perf, "perf executable missing")
    dest = Path(tempfile.mkdtemp(prefix=f"capture-{proc.name}-", dir=PRIVATE))
    perf_env = dict(os.environ, DEBUGINFOD_URLS="", PERF_PAGER="cat", PAGER="cat", LC_ALL="C",
                    PERF_BUILDID_DIR=str(BASE / "diagnostics/private-perf/buildid-cache"))
    def tool(args, out, err, seconds):
        with (dest / out).open("xb") as stdout, (dest / err).open("xb") as stderr:
            cmd = subprocess.run([perf, *args], stdout=stdout, stderr=stderr, env=perf_env, timeout=seconds)
        require(cmd.returncode == 0, f"perf {args[0]} failed, see private {err}")
    jit_map = PRIVATE / f"perf-{proc.name}.map"
    require(jit_map.is_file() and not jit_map.is_symlink(), "JIT map missing")
    known_before = match_pages()
    require(stat.S_IMODE(jit_map.stat().st_mode) & 0o077 == 0, "Map file readable by other users")
    with watch_generation(jit_map):
        freeze_prefix(jit_map, dest / "before.map")
        start = check_game()
        print("Starting 20-second stationary CPU recording", file=sys.stderr, flush=True)
        tool(["record", "-e", "cycles:u", "-F", "199", "-p", proc.name, "-o", str(dest / "perf.data"),
              "--", "sleep", "20"], "record.out", "record.err", 35)
        print("CPU recording finished; owner can move", file=sys.stderr, flush=True)
        end = check_game()
        require(end > start, "Title did not present frames")
        freeze_prefix(jit_map, dest / "after.map")
    known_after = match_pages()
    require(identity(proc) == ident, "Owner process identity changed")
    require(check_game() >= end, "Session changed")
    known = [a and b for a,b in zip(known_before, known_after)]
    record = (dest / "record.err").read_text()
    count = re.search(r"\(([\d,]+) samples\)", record)
    require(count, "perf did not report the sample count")
    tool(["report", "--stdio", "--no-children", "--call-graph", "none", "--sort", "comm",
          "--percent-limit", "100", "-i", str(dest / "perf.data")], "accounting", "accounting.err", 35)
    accounting = (dest / "accounting").read_text()
    lost = re.search(r"^# Total Lost Samples:\s*([\d,]+)\s*$", accounting, re.M)
    weight_header = re.search(r"^# Event count \(approx\.\):\s*([\d,]+)\s*$", accounting, re.M)
    require(lost and int(lost[1].replace(",", "")) == 0, "Perf sample loss or unrecognized report")
    require(weight_header, "Missing perf event weight")
    tool(["script", "-F", "comm,tid,period,ip", "-i", str(dest / "perf.data")], "samples", "script.err", 40)
    rows = read_samples(dest / "samples")
    require(len(rows) == int(count[1].replace(",", "")), "perf record/sample counts differ")
    require(sum(weight for _, _, weight, _ in rows) == int(weight_header[1].replace(",", "")),
            "Period-weighted sum differs from perf accounting")
    prev = dest / "before.map"
    ips = sorted({ip for _, _, _, ip in rows})
    found, uncertain, sha, prefix_rows = matching_ranges(dest / "after.map", ips, prev.stat().st_size)
    with prev.open("rb") as f:
        actual = hashlib.sha256()
        for chunk in iter(lambda: f.read(1<<20), b""):
            actual.update(chunk)
    require(actual.hexdigest() == sha, "JIT map changed during capture")
    result = {"source": SOURCE, "title_id": TITLE, "pid": int(proc.name), "pid_start_ticks": ident[0],
              "parsed_samples": len(rows),
              "lost_records": 0, "duration_s": 20, "map_rows_before": prefix_rows, "present_delta": end - start,
              "private_capture": dest.name, "measure": "sampled process user cycles during named-JIT profiling"}
    blocks = report(result, rows, found, uncertain, known)
    (dest / "blocks.json").write_text(json.dumps(blocks, indent=2))
    (dest / "summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    run()
