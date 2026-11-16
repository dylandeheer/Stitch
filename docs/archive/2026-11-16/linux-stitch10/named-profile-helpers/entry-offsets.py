"""Owner-local counter for x64 entry instructions across A64 blocks. No addresses exported."""
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import runpy
import sys

api = runpy.run_path(str(Path(__file__).with_name("collect-named.py")))


def collect(before, after, rows):
    ips = sorted({ip for _, _, _, ip in rows})
    found, ambiguous, key, _ = api["matching_ranges"](after, ips, before.stat().st_size)
    check = hashlib.sha256()
    offsets = {}
    with before.open("rb") as f:
        for line in f:
            check.update(line)
            a, n, name = line.split(None, 2)
            start, size = int(a, 16), int(n, 16)
            i = bisect.bisect_left(ips, start)
            j = bisect.bisect_left(ips, start + size, i)
            for ip in ips[i:j]:
                if ip not in ambiguous and api["BLOCK"].fullmatch(found.get(ip, "")):
                    offset = ip - start
                    if ip in offsets and offsets[ip] != offset:
                        ambiguous.add(ip)
                    offsets[ip] = offset
    api["require"](key == check.hexdigest(), "Prefix mismatch")
    total = sum(w for _, _, w, _ in rows)
    counts, samples = Counter(), Counter()
    for _, _, w, ip in rows:
        if ip in ambiguous or ip not in offsets:
            continue
        offset = offsets[ip]
        label = {0: "entry MOV saves RBP", 8: "entry LEA sets RBP"}.get(offset, "other A64 host IP")
        counts[label] += w
        samples[label] += 1
    return {"sampled_process_cycles": total, "sampled_process_records": len(rows),
            "entry_buckets": [{"instruction": k, "sampled_records": samples[k], "sampled_cycles": w,
                              "pct_process_cycles": 100 * w / total} for k, w in counts.most_common()],
            "scope": "Sampled IP attribution includes PMU skid and does not measure causal stall or removable cost."}


if __name__ == "__main__":
    p = Path(sys.argv[1])
    meta = json.loads((p / "summary.json").read_text())
    api["require"](meta["source"] == api["SOURCE"] and meta["lost_records"] == 0, "Capture identity or loss")
    print(json.dumps(collect(p / "before.map", p / "after.map", api["read_samples"](p / "samples")), indent=2))
