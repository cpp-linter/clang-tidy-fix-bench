#!/usr/bin/env python3
"""One table across runs (agent x condition), from each run's results.jsonl.

usage: compare.py <run> [<run> ...]   -> runs/compare.md

Run score.py <run> --report (with the same --judges for every run) first.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def pct(k, n):
    return f"{k}/{n} ({k / n:.0%})" if n else "-"


def judged(rows):
    """Judge names; agreement only means something with two or more judges."""
    names = sorted({j for r in rows for j in r.get("judges") or {}})
    if len(names) < 2:
        return f"{names[0]} (single judge)" if names else "-"
    return f"{', '.join(names)}: {pct(sum(r['verdict'] != 'disputed' for r in rows), len(rows))} agree"


def main(runs):
    lines = ["| Run | Agent | Model | Checker | Hint | Samples | Mechanical pass | Accepted by judges | Success | use-after-move success | Judges (agreement) | Audit flags |",
             "|---|---|---|:---:|:---:|---:|---:|---:|---:|---:|---:|---:|"]
    for run in runs:
        rd = ROOT / "runs" / run
        cfg = json.loads((rd / "config.json").read_text())
        ag = [json.loads(l) for l in (rd / "agent.jsonl").read_text().splitlines()] if (rd / "agent.jsonl").exists() else []
        rows = [json.loads(l) for l in (rd / "results.jsonl").read_text().splitlines()]
        n = len(rows)
        uam = [r for r in rows if r["check"] == "bugprone-use-after-move"]
        agent = ag[0]["agent"] if ag else "?"
        # the model that answered (run_agents.py records it), else the one requested
        answered = sorted({m for a in ag for m in a.get("models", [])})
        model = ", ".join(answered) or ((ag[0].get("model") or "default") if ag else "?")
        lines.append(" | ".join([
            f"| {run}", agent, model, "yes" if cfg["checker"] else "no", "yes" if cfg["hint"] else "no", str(n),
            pct(sum(bool(r.get("resolved")) for r in rows), n), pct(sum(r["accepted"] for r in rows), n),
            pct(sum(r["success"] for r in rows), n), pct(sum(r["success"] for r in uam), len(uam)),
            judged(rows),
            str(sum(bool(a["audit"]) for a in ag)) + " |"]))
    out = ROOT / "runs" / "compare.md"
    out.write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1:])
