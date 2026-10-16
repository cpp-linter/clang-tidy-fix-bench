#!/usr/bin/env python3
"""Create one task directory per sample under runs/<run>/tasks/<id>/.

Each task holds only what a fix agent would see in production: the file before
the fix, the warnings to fix, and a check command. The human fix stays in
data/samples and is never copied here.
"""
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"

TEMPLATE = """# Fix clang-tidy warnings

Repository: {repo} (file `{file}`)
Check: `{check}`

The file to edit is `{local}` in this directory. clang-tidy reports these
warnings in it, and they should be fixed:

{warnings}

Rules:
- Edit only `{local}`. Make the smallest change that fixes the warnings
  properly, keeping behaviour the same unless the warning points at a real bug.
- Do not silence the check (no `NOLINT`, no pragmas, no config changes).
- You can check your edit with:
  `python3 {checker} {sid} {local_abs}`
  The build setup is approximate, so compiler errors that were already there
  are expected; only new errors matter.
- When done, write a one-paragraph explanation of the change to `{note}`.
"""


def main(run):
    td = ROOT / "runs" / run / "tasks"
    td.mkdir(parents=True, exist_ok=True)
    n = 0
    for mp in sorted(SAMPLES.glob("*/meta.json")):
        m = json.loads(mp.read_text())
        d = td / m["id"]
        if d.exists():
            continue
        d.mkdir()
        local = Path(m["file"]).name
        shutil.copy(mp.parent / f"before{Path(m['file']).suffix}", d / local)
        ws = "\n".join(f"- line {w['line']}, col {w['col']}: {w['msg']} [{m['check_name']}]" for w in m["warnings_fixed"])
        (d / "TASK.md").write_text(TEMPLATE.format(
            repo=m["repo"], file=m["file"], check=m["check_name"], local=local, warnings=ws,
            checker=ROOT / "scripts" / "check_fix.py", sid=m["id"], local_abs=d / local, note=d / "NOTE.md"))
        n += 1
    print(f"{n} tasks created in {td}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "run1")
