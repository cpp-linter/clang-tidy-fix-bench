#!/usr/bin/env python3
"""Score a run: mechanical checks per task, then (optionally) merge judge verdicts.

usage: score.py <run>            -> runs/<run>/scores.jsonl (mechanical) + review packets
       score.py <run> --report   -> runs/<run>/report.md (needs judge/<id>.json files)
"""
import difflib
import fnmatch
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import check_fix  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"


def udiff(a, b, name):
    return "".join(difflib.unified_diff(a.splitlines(True), b.splitlines(True), f"a/{name}", f"b/{name}", n=2))


NOLINT = re.compile(r"NOLINT(?:NEXTLINE|BEGIN)?(?:\(([^)]*)\))?")


def suppressions(lines, names):
    """NOLINT markers in `lines` that would silence any of `names`."""
    n = 0
    for l in lines:
        for m in NOLINT.finditer(l):
            pats = [x.strip() for x in (m.group(1) or "*").split(",")]
            n += any(fnmatch.fnmatch(c, pat) for c in names for pat in pats)
    return n


def added_suppression(patch, names):
    """True if the edit adds a NOLINT covering the target check (pre-existing
    markers for other checks that merely sit on an edited line do not count)."""
    plus = [l[1:] for l in patch.splitlines() if l.startswith("+") and not l.startswith("+++")]
    minus = [l[1:] for l in patch.splitlines() if l.startswith("-") and not l.startswith("---")]
    return suppressions(plus, names) > suppressions(minus, names)


def changed(p):
    return sum(1 for l in p.splitlines() if l[:1] in "+-" and l[:3] not in ("+++", "---"))


def mechanical(run):
    rd = ROOT / "runs" / run
    out = (rd / "scores.jsonl").open("w")
    (rd / "review").mkdir(exist_ok=True)
    for td in sorted((rd / "tasks").iterdir()):
        m = json.loads((SAMPLES / td.name / "meta.json").read_text())
        ext = Path(m["file"]).suffix
        local = Path(m["file"]).name
        before = (SAMPLES / td.name / f"before{ext}").read_text()
        edited = (td / local).read_text()
        rec = {"id": td.name, "check": m["check"], "repo": m["repo"], "license": m["license"]}
        if edited == before:
            rec.update(status="no-edit", resolved=False)
        else:
            r = check_fix.evaluate(td.name, edited)
            p = udiff(before, edited, m["file"])
            nolint = added_suppression(p, {m["check"], m["check_name"]})
            rec.update(status="edited", fixed=r["fixed_target"], total=r["total_target"],
                       new_warnings=len(r["new_warnings"]), errors=r["errors"], errors_before=r["errors_before"],
                       out_of_scope=len(r["out_of_scope_edits"]), nolint=nolint,
                       agent_changed=changed(p), human_changed=m["human_changed_lines"])
            rec["resolved"] = (r["fixed_target"] == r["total_target"] and not r["new_warnings"]
                               and r["errors"] <= r["errors_before"] and not r["out_of_scope_edits"] and not nolint)
            (td / "agent.patch").write_text(p)
            note = (td / "NOTE.md").read_text() if (td / "NOTE.md").exists() else ""
            (rd / "review" / f"{td.name}.md").write_text(
                f"# {td.name}\n\nCheck: {m['check_name']}\nRepo: {m['repo']} ({m['url']})\n\n"
                f"## Warnings\n" + "\n".join(f"- line {w['line']}: {w['msg']}" for w in m["warnings_fixed"]) +
                f"\n\n## Human fix (merged upstream)\n```diff\n{(SAMPLES / td.name / 'human.patch').read_text()}```\n\n"
                f"## Candidate fix\n```diff\n{p}```\n\nCandidate's note: {note.strip()}\n\n"
                f"## Mechanical result\n{json.dumps({k: rec[k] for k in ('fixed', 'total', 'new_warnings', 'errors', 'errors_before', 'out_of_scope', 'nolint')})}\n")
        out.write(json.dumps(rec) + "\n")
        print(rec["id"], rec.get("status"), rec.get("resolved"), flush=True)


def pct(k, n):
    return f"{k} ({k / n:.0%})"


def report(run):
    rd = ROOT / "runs" / run
    rows = [json.loads(l) for l in (rd / "scores.jsonl").read_text().splitlines()]
    by = defaultdict(list)
    for r in rows:
        j = rd / "judge" / f"{r['id']}.json"
        jd = json.loads(j.read_text()) if j.exists() else {}
        r["verdict"], r["judge_reason"] = jd.get("verdict"), jd.get("reason")
        r["accepted"] = r["verdict"] in ("equivalent", "acceptable")
        r["success"] = bool(r.get("resolved")) and r["accepted"]
        by[r["check"]].append(r)
    lines = ["| Check | Samples | Mechanical pass | Judge: equivalent to human | Judge: equivalent or acceptable | Success (mechanical and judge) |",
             "|---|---:|---:|---:|---:|---:|"]
    tot = defaultdict(int)
    for c, rs in sorted(by.items()):
        n = len(rs)
        v = dict(n=n, a=sum(bool(r.get("resolved")) for r in rs), e=sum(r["verdict"] == "equivalent" for r in rs),
                 b=sum(r["accepted"] for r in rs), s=sum(r["success"] for r in rs))
        for k, x in v.items():
            tot[k] += x
        lines.append(f"| `{c}` | {n} | {pct(v['a'], n)} | {pct(v['e'], n)} | {pct(v['b'], n)} | {pct(v['s'], n)} |")
    n = tot["n"]
    lines.append(f"| **All** | {n} | {pct(tot['a'], n)} | {pct(tot['e'], n)} | {pct(tot['b'], n)} | {pct(tot['s'], n)} |")
    (rd / "report.md").write_text("\n".join(lines) + "\n")
    (rd / "results.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
    print("\n".join(lines))


if __name__ == "__main__":
    run = sys.argv[1]
    report(run) if "--report" in sys.argv else mechanical(run)
