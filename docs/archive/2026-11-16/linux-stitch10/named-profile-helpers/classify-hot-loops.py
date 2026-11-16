"""Optional, after one stable capture. Classify narrow A64 loop shapes on three hot pages."""
import collections
import hashlib
import json
from pathlib import Path
import runpy
import sys

api = runpy.run_path(str(Path(__file__).with_name("collect-named.py")))


def loop_kind(words):
    if len(words) < 2:
        return None
    load, branch = words[:2]
    dest, base = load & 31, (load >> 5) & 31
    plain_load = load & 0x3fc00000 == 0x39400000  # unsigned-offset scalar LDR(B/H)
    acquire_load = load & 0x3ffffc00 == 0x08dffc00  # LDAR(B/H)
    if (plain_load or acquire_load) and dest != 31 and dest != base and branch & 31 == dest:
        opcode = branch & 0x7e000000
        if ((opcode == 0x34000000 and (branch >> 5) & 0x7ffff == 0x7ffff) or
            (opcode == 0x36000000 and (branch >> 5) & 0x3fff == 0x3fff)):
            return "conditional re-read of a fixed address"
    if len(words) < 3 or load & 0xffffffe0 not in (0xd53be020, 0xd53be040) or dest == 31:
        return None
    cmp, cond = words[1:3]
    # Exact unshifted 64-bit CMP Xn,Xm. One operand is the counter, the other stays unchanged.
    lhs, rhs = (cmp >> 5) & 31, (cmp >> 16) & 31
    if (cmp & 0xffe0fc1f == 0xeb00001f and (lhs == dest) != (rhs == dest) and
            cond & 0xff000010 == 0x54000000 and (cond >> 5) & 0x7ffff == 0x7fffe and
            cond & 15 < 14):
        return "counter deadline comparison"
    return None


def run(folder):
    req = api["require"]
    req(folder.parent.resolve(strict=True) == api["PRIVATE"] and not folder.is_symlink(), "Wrong private capture")
    s = json.loads((folder / "summary.json").read_text())
    req(s["source"] == api["SOURCE"] and s["title_id"] == api["TITLE"] and s["lost_records"] == 0, "Wrong capture")
    proc = Path("/proc") / str(s["pid"])
    req(api["identity"](proc)[0] == s["pid_start_ticks"], "Game process has changed")
    req(hashlib.sha256((proc / "exe").read_bytes()).hexdigest() == api["EXE_SHA"], "Different executable")
    api["check_game"]()
    pages = collections.Counter()
    weights = {}
    for block in json.loads((folder / "blocks.json").read_text()):
        pc = int(block["guest_pc"], 16)
        weights[pc] = block["weight"]
        page = pc & ~4095
        known = any(page == loc and s["matched_historical_pages"][desc] for loc, _, _, _, desc in api["PAGES"])
        if not known:
            pages[page] += block["weight"]
    totals = collections.Counter()
    inspected = 0
    examined = 0
    for page, _ in pages.most_common(3):
        raw = bytes.fromhex(api["rpc"]("read_memory", {"address": hex(page), "size": 4096})["hex"])
        req(len(raw) == 4096, "Incomplete page")
        examined += 1
        for pc, weight in weights.items():
            if pc & ~4095 != page:
                continue
            pos = pc - page
            words = [int.from_bytes(raw[i:i+4], "little") for i in range(pos, min(pos+12, 4096), 4)]
            kind = loop_kind(words)
            if kind:
                totals[kind] += weight
            inspected += weight
    api["check_game"]()
    req(api["identity"](proc)[0] == s["pid_start_ticks"], "Game process changed during read")
    print(json.dumps({"pages_examined": examined, "pct_process_inspected": 100*inspected/s["estimated_cycles"],
                      "pct_process_by_exact_loop_shape": {k:100*v/s["estimated_cycles"] for k,v in totals.items()},
                      "attribution": "Post-capture code inspection of mapped block starts; a conditional back edge shows loop structure, not observed branch outcomes or frame-critical idle time."}, indent=2))


if __name__ == "__main__":
    run(Path(sys.argv[1]))
