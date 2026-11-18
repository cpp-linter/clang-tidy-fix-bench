#!/usr/bin/env python3
"""Create one task directory per sample under runs/<run>/tasks/<id>/.

Each task holds only what a fix agent would see in production: the file before
the fix, the warnings to fix and, depending on the options, a check command
and a remediation hint. The human fix stays in samples/ and is never copied.

usage: make_tasks.py <run> [--no-checker] [--hint] [--subset pilot|all]

  --no-checker  do not offer check_fix.py (single attempt, no tool feedback)
  --hint        add the per-check remediation hint from hints/<check>.md,
                the kind of text a linter could print next to a warning
  --subset      pilot = the ids in data/pilot.txt (2 per check); default all

run1 used the defaults (checker offered, no hint).
"""
import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"
HINTS = ROOT / "hints"

HEAD = """# Fix clang-tidy warnings

Repository: {repo} (file `{file}`)
Check: `{check}`

The file to edit is `{local}` in this directory. clang-tidy reports these
warnings in it, and they should be fixed:

{warnings}
"""

HINT = """
How to fix warnings from this check:

{hint}
"""

RULES = """
Rules:
- Edit only `{local}`. Make the smallest change that fixes the warnings
  properly, keeping behaviour the same unless the warning points at a real bug.
- Do not silence the check (no `NOLINT`, no pragmas, no config changes).
"""

CHECKER = """- You can check your edit with:
  `python3 {checker} {sid} {local_abs}`
  The build setup is approximate, so compiler errors that were already there
  are expected; only new errors matter.
"""

NO_CHECKER = """- You cannot run clang-tidy or a compiler for this task; work from the file
  and the warnings. Do not look for other copies of this file or its fix.
"""

NOTE = """- When done, write a one-paragraph explanation of the change to `NOTE.md`
  in this directory.
"""


def pilot_ids():
    return set((ROOT / "data" / "pilot.txt").read_text().split())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--no-checker", action="store_true")
    ap.add_argument("--hint", action="store_true")
    ap.add_argument("--subset", choices=("all", "pilot"), default="all")
    a = ap.parse_args()
    rd = ROOT / "runs" / a.run
    td = rd / "tasks"
    td.mkdir(parents=True, exist_ok=True)
    keep = pilot_ids() if a.subset == "pilot" else None
    n = 0
    for mp in sorted(SAMPLES.glob("*/meta.json")):
        m = json.loads(mp.read_text())
        if keep is not None and m["id"] not in keep:
            continue
        d = td / m["id"]
        if d.exists():
            continue
        d.mkdir()
        local = Path(m["file"]).name
        shutil.copy(mp.parent / f"before{Path(m['file']).suffix}", d / local)
        ws = "\n".join(f"- line {w['line']}, col {w['col']}: {w['msg']} [{m['check_name']}]" for w in m["warnings_fixed"])
        text = HEAD.format(repo=m["repo"], file=m["file"], check=m["check_name"], local=local, warnings=ws)
        if a.hint:
            text += HINT.format(hint=(HINTS / f"{m['check']}.md").read_text().strip())
        text += RULES.format(local=local)
        if a.no_checker:
            text += NO_CHECKER
        else:
            text += CHECKER.format(checker=ROOT / "scripts" / "check_fix.py", sid=m["id"], local_abs=d / local)
        text += NOTE
        (d / "TASK.md").write_text(text)
        n += 1
    (rd / "config.json").write_text(json.dumps(
        {"checker": not a.no_checker, "hint": a.hint, "subset": a.subset}, indent=1) + "\n")
    print(f"{n} tasks created in {td}")


if __name__ == "__main__":
    main()
