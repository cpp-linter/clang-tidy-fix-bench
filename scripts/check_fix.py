#!/usr/bin/env python3
"""Run the sample's clang-tidy check on a candidate version of its file.

usage: check_fix.py <sample-id> <path-to-edited-file> [--json]

Prints the target-check warnings that remain in the edited file, which of the
warnings the task asked to fix are still there, and new warnings/errors.
"""
import difflib
import fcntl
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import tidy  # noqa: E402

SAMPLES = tidy.ROOT / "samples"
# How far (in lines of the original file) an edit may sit from a target warning.
# Same windows validate.py uses to accept the human fix.
NEAR = {"cppcoreguidelines-special-member-functions": 60}
DEFAULT_NEAR = 15


def edited_ranges(before, text):
    """Line ranges (1-based, in `before`) touched by the edit."""
    a, b = before.split("\n"), text.split("\n")
    out = []
    for tag, i1, i2, _, _ in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag != "equal":
            out.append((i1 + 1, max(i2, i1 + 1)))
    return out


def out_of_scope(meta, before, text):
    """Edited ranges that are not within NEAR lines of any target warning."""
    near = NEAR.get(meta["check"], DEFAULT_NEAR)
    lines = [w["line"] for w in meta["warnings_fixed"]]
    return [r for r in edited_ranges(before, text) if not any(r[0] - near <= l <= r[1] + near for l in lines)]


def _analyze(meta, text):
    lock = tidy.ROOT / "cache" / f"{meta['repo'].replace('/', '__')}.lock"
    lock.parent.mkdir(exist_ok=True)
    with open(lock, "w") as lf:
        fcntl.flock(lf, fcntl.LOCK_EX)
        d = tidy.checkout(meta["repo"], meta["sha"], [meta["file"]])
        tidy.switch(d, meta["parent"])
        missing = set(meta["missing"])
        warns, errs = tidy.analyze(d, meta["file"], text, meta["check_name"], meta["flags"], missing)
        tidy.switch(d, meta["parent"])
    return warns, errs


def reference(meta, before):
    """Warnings and error count of the unedited file under this exact harness
    (cached), so a candidate is compared like with like."""
    f = tidy.ROOT / "cache" / "reference" / f"{meta['id']}.json"
    if f.exists():
        return json.loads(f.read_text())
    warns, errs = _analyze(meta, before)
    ref = {"warnings": warns, "errors": errs}
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps(ref))
    return ref


def evaluate(sid, text):
    meta = json.loads((SAMPLES / sid / "meta.json").read_text())
    before = (SAMPLES / sid / f"before{Path(meta['file']).suffix}").read_text()
    ref = reference(meta, before)
    warns, errs = _analyze(meta, text)
    lines = before.split("\n")
    key = lambda w: (w["msg"], lines[w["line"] - 1].strip())  # noqa: E731
    # Multisets: the same message can sit on several identical lines.
    target = Counter(key(w) for w in meta["warnings_fixed"])
    kb = tidy.keyed(ref["warnings"], before)
    ka = tidy.keyed(warns, text)
    gone, new = tidy.diff_keys(kb, ka)
    gone_c = Counter(key(w) for w in gone)
    fixed = sum(min(n, gone_c[k]) for k, n in target.items())
    remaining = []
    for k, n in target.items():
        left = n - min(n, gone_c[k])
        remaining += [w for k2, w in kb if k2 == k][:left]
    have = Counter(k for k, _ in kb)
    return {
        "remaining_target": [{"line": w["line"], "msg": w["msg"]} for w in remaining],
        "fixed_target": fixed, "total_target": sum(target.values()),
        "new_warnings": [{"line": w["line"], "msg": w["msg"]} for w in new],
        "errors": errs, "errors_before": ref["errors"],
        "harness_ok": all(have[k] >= n for k, n in target.items()),
        "out_of_scope_edits": out_of_scope(meta, before, text),
        "all_warnings_now": [{"line": w["line"], "msg": w["msg"]} for w in warns],
    }


if __name__ == "__main__":
    sid, path = sys.argv[1], sys.argv[2]
    r = evaluate(sid, Path(path).read_text())
    if "--json" in sys.argv:
        print(json.dumps(r))
    else:
        print(f"target warnings fixed: {r['fixed_target']}/{r['total_target']}")
        for w in r["remaining_target"]:
            print(f"  still present (originally line {w['line']}): {w['msg']}")
        for w in r["new_warnings"]:
            print(f"  NEW warning at line {w['line']}: {w['msg']}")
        for a, b in r["out_of_scope_edits"]:
            print(f"  edit at original lines {a}-{b} is far from every target warning")
        print(f"compiler errors in file: {r['errors']} (before your edit: {r['errors_before']}; "
              "these come from the approximate build setup, only an increase matters)")
