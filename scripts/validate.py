#!/usr/bin/env python3
"""Turn candidate commits into validated samples.

A (commit, file) pair becomes a sample when, with the target check only:
  * the parent version has at least one warning that is gone after the commit,
  * the commit adds no new warning of that check to the file,
  * every diff hunk in the file sits near a warning that went away
    (so the change is about this warning, not unrelated edits).
"""
import fcntl
import json
import re
import subprocess
import sys
import traceback
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import tidy  # noqa: E402

ROOT = tidy.ROOT
CANDS = ROOT / "data" / "candidates.jsonl"
SAMPLES = ROOT / "samples"
LOG = ROOT / "data" / "validate.jsonl"
NEAR = {"cppcoreguidelines-special-member-functions": 60}
MAX_PER_REPO = 4
MAX_PER_CHECK = 25


def hunks(d, parent, sha, rel):
    out = tidy.git(d, "diff", "-U0", parent, sha, "--", rel).stdout
    hs = []
    for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", out, re.M):
        start, n = int(m.group(1)), int(m.group(2) or 1)
        hs.append((start, start + max(n, 1) - 1))
    return hs, out


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    done = set()
    if LOG.exists():
        for line in LOG.read_text().splitlines():
            r = json.loads(line)
            done.add((r["sha"], r["file"]))
    per_repo = defaultdict(int)
    per_check = defaultdict(int)
    for p in SAMPLES.glob("*/meta.json"):
        m = json.loads(p.read_text())
        per_repo[(m["check"], m["repo"])] += 1
        per_check[m["check"]] += 1
    log = LOG.open("a")
    cands = [json.loads(l) for l in CANDS.read_text().splitlines()]
    # smaller commits first: more likely to be a focused fix
    cands.sort(key=lambda c: c["changes"])
    for c in cands:
        if only and c["check"] != only:
            continue
        if per_check[c["check"]] >= MAX_PER_CHECK or per_repo[(c["check"], c["repo"])] >= MAX_PER_REPO:
            continue
        lf = open(tidy.ROOT / "cache" / f"{c['repo'].replace('/', '__')}.lock", "w")
        fcntl.flock(lf, fcntl.LOCK_EX)
        try:
            process(c, done, per_repo, per_check, log)
        finally:
            lf.close()


def process(c, done, per_repo, per_check, log):
    if True:
        try:
            d = tidy.checkout(c["repo"], c["sha"], c["files"])
            tidy.switch(d, c["parent"])
        except Exception as e:
            for f in c["files"]:
                log.write(json.dumps({"sha": c["sha"], "file": f, "check": c["check"], "status": "fetch-failed", "err": str(e)[:200]}) + "\n")
            return
        names = tidy.git(d, "ls-tree", "-r", "--name-only", c["parent"]).stdout.split("\n")
        repo_is_cpp = any(n.endswith((".cpp", ".cc", ".cxx", ".hpp")) for n in names)
        for rel in c["files"]:
            if (c["sha"], rel) in done:
                continue
            rec = {"sha": c["sha"], "file": rel, "check": c["check"], "repo": c["repo"]}
            try:
                before = tidy.git(d, "show", f"{c['parent']}:{rel}").stdout
                after = tidy.git(d, "show", f"{c['sha']}:{rel}").stdout
                flags = tidy.lang_flags(rel, repo_is_cpp)
                missing = set()
                tidy.switch(d, c["parent"])
                wb, eb = tidy.analyze(d, rel, before, c["matched_name"], flags, missing)
                tidy.switch(d, c["sha"])
                wa, ea = tidy.analyze(d, rel, after, c["matched_name"], flags, missing)
                gone, new = tidy.diff_keys(tidy.keyed(wb, before), tidy.keyed(wa, after))
                hs, patch = hunks(d, c["parent"], c["sha"], rel)
                near = NEAR.get(c["check"], 15)
                unrelated = [h for h in hs if not any(h[0] - near <= w["line"] <= h[1] + near for w in gone)]
                rec.update(before=len(wb), after=len(wa), gone=len(gone), new=len(new), errors_before=eb,
                           errors_after=ea, hunks=len(hs), unrelated_hunks=len(unrelated), missing=len(missing))
                ok = gone and not new and not unrelated
                rec["status"] = "ok" if ok else ("no-warning" if not wb else "not-fixed" if not gone else "new-warnings" if new else "unrelated-edits")
                if ok:
                    sid = f"{c['check']}__{c['repo'].replace('/', '__')}__{c['sha'][:10]}__{Path(rel).name}"
                    sd = SAMPLES / sid
                    sd.mkdir(parents=True, exist_ok=True)
                    ext = Path(rel).suffix
                    (sd / f"before{ext}").write_text(before)
                    (sd / f"after{ext}").write_text(after)
                    (sd / "human.patch").write_text(patch)
                    rel_w = lambda ws: [{k: v for k, v in w.items() if k != "file"} for w in ws]  # noqa: E731
                    meta = dict(id=sid, check=c["check"], check_name=c["matched_name"], repo=c["repo"], sha=c["sha"],
                                parent=c["parent"], url=c["url"], license=c["license"], message=c["message"], file=rel,
                                flags=flags, missing=sorted(missing), warnings_before=rel_w(wb), warnings_fixed=rel_w(gone),
                                errors_before=eb, human_changed_lines=sum(1 for l in patch.splitlines() if l[:1] in "+-" and l[:3] not in ("+++", "---")))
                    (sd / "meta.json").write_text(json.dumps(meta, indent=1))
                    per_repo[(c["check"], c["repo"])] += 1
                    per_check[c["check"]] += 1
            except Exception as e:
                rec["status"] = "error"
                rec["err"] = traceback.format_exc()[-300:]
            log.write(json.dumps(rec) + "\n")
            log.flush()
            print(rec.get("status"), c["check"], c["repo"], rel, flush=True)
            if per_check[c["check"]] >= MAX_PER_CHECK:
                return


if __name__ == "__main__":
    main()
