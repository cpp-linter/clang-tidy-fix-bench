#!/usr/bin/env python3
"""Blind judge review of a scored run with a coding-agent CLI.

usage: judge.py <run> --judge claude|codex|copilot [--model M] [--jobs 4] [--limit N]
  -> runs/<run>/judge-<judge>/<id>.json  {"verdict": ..., "reason": ...}

Needs score.py <run> first (it writes the review packets). The judge sees the
warnings, the human fix, the candidate fix, the before/after/candidate files
and the mechanical result, but never which agent or condition produced the
candidate. Use two judges from different vendors and report both; a verdict
is final only where they agree (score.py --report --judges a,b).
"""
import argparse
import json
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from run_agents import clean_env

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"

RUBRIC = """# Review one candidate fix

`packet.md` shows clang-tidy warnings in one file, the fix a maintainer merged
upstream, and a candidate fix. `before.*` is the file before any fix,
`human.*` is the file after the maintainer's fix, `candidate.*` is the file
after the candidate fix.

Give the candidate exactly one verdict:
- "equivalent": the same semantic change as the human fix.
- "acceptable": a different fix that is still correct and that a maintainer
  could merge.
- "incorrect": changes behaviour unintentionally, only gets past the checker
  (for example reads a moved-from object through an alias), changes what a test
  asserts, or would be rejected for another reason.
- "no-fix": the candidate did not change the code in a meaningful way.

Do not edit any file. Answer with one JSON object and nothing else:
{"verdict": "...", "reason": "one or two sentences"}
"""
VERDICTS = ("equivalent", "acceptable", "incorrect", "no-fix")
PROMPT = "Read JUDGE.md and packet.md in the current directory and answer as JUDGE.md says."


def command(judge, model, ws):
    """Read-only judges with personal config kept out (see run_agents.py)."""
    if judge == "claude":
        return ["claude", "-p", PROMPT, "--output-format", "json", "--safe-mode", "--setting-sources", "",
                "--no-session-persistence", "--tools", "Read"] + (["--model", model] if model else [])
    if judge == "codex":
        return ["codex", "exec", "--sandbox", "read-only", "--skip-git-repo-check", "--ephemeral",
                "-c", "notify=[]", "-C", str(ws)] + (["-m", model] if model else []) + [PROMPT]
    if judge == "copilot":
        return ["copilot", "-p", PROMPT, "--no-ask-user", "--output-format", "json", "--no-custom-instructions",
                "--disable-builtin-mcps", "--no-auto-update", "--available-tools=view,glob,grep",
                "--deny-tool=write", "--deny-tool=url"] + (["--model", model] if model else [])
    raise SystemExit(f"unknown judge {judge}")


def answer(judge, out):
    """The judge's final text and the model(s) that produced it."""
    if judge == "codex":
        return out, []
    text, models = "", set()
    for line in out.splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if not isinstance(d, dict):
            continue
        if judge == "claude" and d.get("type") == "result":
            text, models = str(d.get("result") or ""), set(d.get("modelUsage") or {})
        elif judge == "copilot" and d.get("type") == "assistant.message":
            data = d.get("data") or {}
            models.add(data.get("model"))
            text = data.get("content") or text
    return text, sorted(m for m in models if m)


def parse(text):
    dec, found = json.JSONDecoder(), None
    for i, ch in enumerate(text):
        if ch != "{":
            continue
        try:
            d, _ = dec.raw_decode(text, i)
        except json.JSONDecodeError:
            continue
        if isinstance(d, dict) and d.get("verdict") in VERDICTS:
            found = {"verdict": d["verdict"], "reason": str(d.get("reason", ""))}
    return found


def one(pk, a, rd, out):
    sid = pk.stem
    dst = out / f"{sid}.json"
    if dst.exists():
        return
    m = json.loads((SAMPLES / sid / "meta.json").read_text())
    ext = Path(m["file"]).suffix
    with tempfile.TemporaryDirectory(prefix="ctfb-judge-") as tmp:
        ws = Path(tmp)
        (ws / "JUDGE.md").write_text(RUBRIC)
        shutil.copy(pk, ws / "packet.md")
        shutil.copy(SAMPLES / sid / f"before{ext}", ws / f"before{ext}")
        shutil.copy(SAMPLES / sid / f"after{ext}", ws / f"human{ext}")
        shutil.copy(rd / "tasks" / sid / Path(m["file"]).name, ws / f"candidate{ext}")
        try:
            p = subprocess.run(command(a.judge, a.model, ws), cwd=ws, env=clean_env(a.judge), capture_output=True,
                               text=True, timeout=a.timeout, stdin=subprocess.DEVNULL)
            text, models = answer(a.judge, p.stdout)
            raw = p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            text, models, raw = "", [], "timeout"
    d = parse(text)
    if d is None:
        (out / f"{sid}.raw.txt").write_text(text or raw)
        print(sid, "UNPARSED", flush=True)
        return
    d.update(judge=a.judge, model=a.model, models=models)
    dst.write_text(json.dumps(d) + "\n")
    print(sid, d["verdict"], flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--judge", required=True, choices=("claude", "codex", "copilot"))
    ap.add_argument("--model")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--limit", type=int)
    a = ap.parse_args()
    rd = ROOT / "runs" / a.run
    out = rd / f"judge-{a.judge}"
    out.mkdir(exist_ok=True)
    packets = sorted((rd / "review").glob("*.md"))[: a.limit]
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(lambda pk: one(pk, a, rd, out), packets))


if __name__ == "__main__":
    main()
