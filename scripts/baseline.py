#!/usr/bin/env python3
"""Sanity-check the harness: score each sample's human fix (after.*) and the
unchanged file (before.*) with the same mechanical checks used for candidates.

usage: baseline.py [workers]   -> results/baseline.jsonl
"""
import json
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import check_fix  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"


def one(mp):
    m = json.loads(mp.read_text())
    ext = Path(m["file"]).suffix
    rec = {"id": m["id"], "check": m["check"]}
    for name in ("before", "after"):
        r = check_fix.evaluate(m["id"], (mp.parent / f"{name}{ext}").read_text())
        rec[name] = {"fixed": r["fixed_target"], "total": r["total_target"], "new_warnings": len(r["new_warnings"]),
                     "errors": r["errors"], "errors_before": r["errors_before"],
                     "out_of_scope": len(r["out_of_scope_edits"]), "harness_ok": r["harness_ok"]}
    a = rec["after"]
    rec["human_resolved"] = (a["fixed"] == a["total"] and not a["new_warnings"]
                             and a["errors"] <= a["errors_before"] and not a["out_of_scope"])
    print(rec["id"], rec["human_resolved"], flush=True)
    return rec


def main(workers):
    out = ROOT / "results" / "baseline.jsonl"
    out.parent.mkdir(exist_ok=True)
    with ThreadPoolExecutor(workers) as ex:
        rows = list(ex.map(one, sorted(SAMPLES.glob("*/meta.json"))))
    out.write_text("".join(json.dumps(r) + "\n" for r in rows))
    print(sum(r["human_resolved"] for r in rows), "/", len(rows), "human fixes pass")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 6)
