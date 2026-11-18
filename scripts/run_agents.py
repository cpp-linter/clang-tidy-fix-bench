#!/usr/bin/env python3
"""Run a coding-agent CLI on every task of a run, one fresh session per task.

usage: run_agents.py <run> --agent claude|codex|copilot|copy-human
                     [--model M] [--jobs 4] [--timeout 900] [--limit N]

Each task directory is copied to a scratch workspace OUTSIDE this repository,
the agent works there with the prompt "Read TASK.md and do what it says", and
the edited file and NOTE.md are copied back into runs/<run>/tasks/<id>/.
Finished tasks are skipped, so the script can be stopped and started again.

Per task it records the CLI version, model, exit code, time and transcript in
runs/<run>/agent.jsonl and runs/<run>/logs/. audit() flags transcripts that
mention the dataset or a tool the condition does not allow; flagged tasks are
reported, not silently dropped.

The claude and copilot flags below were checked with Claude Code 2.1.311 and
Copilot CLI 1.0.73 (run2); the codex flags are untested. Run `--limit 1` first
and read runs/<run>/logs/ before a full run. copilot needs
runs/.copilot-home/config.json holding only the login entries of
~/.copilot/config.json (see COPILOT_HOME). copy-human is a test agent that
copies the human fix (plumbing check only).
"""
import argparse
import json
import os
import shutil
import subprocess
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SAMPLES = ROOT / "samples"
PROMPT = ("Read TASK.md in the current directory and do what it says. "
          "Work only in this directory.")

# Tools each CLI may use. With the checker offered, a shell is needed to run it.
# Personal config (CLAUDE.md, AGENTS.md, skills, MCP servers) is kept out of the
# runs: claude --safe-mode --setting-sources "" (user settings set e.g. a reply
# language); copilot gets its own COPILOT_HOME (no user hooks or skills; the
# login stays in the keychain), --no-custom-instructions, --disable-builtin-mcps.
COPILOT_HOME = ROOT / "runs" / ".copilot-home"
COPILOT_FILE_TOOLS = "view,create,edit,glob,grep"

def command(agent, model, ws, checker):
    if agent == "claude":
        tools = ["Read", "Edit", "Write"] + (["Bash"] if checker else [])
        cmd = ["claude", "-p", PROMPT, "--output-format", "stream-json", "--verbose", "--permission-mode", "acceptEdits",
               "--safe-mode", "--setting-sources", "", "--no-session-persistence", "--tools", ",".join(tools)]
        if checker:
            cmd += ["--allowedTools", "Bash(python3:*)"]
        return cmd + (["--model", model] if model else [])
    if agent == "codex":
        cmd = ["codex", "exec", "--sandbox", "workspace-write", "--skip-git-repo-check", "--ephemeral",
               "--json", "-c", "notify=[]", "-C", str(ws), "-o", str(ws / ".last-message.txt")]
        return cmd + (["-m", model] if model else []) + [PROMPT]
    if agent == "copilot":
        cmd = ["copilot", "-p", PROMPT, "--no-ask-user", "--output-format", "json", "--no-custom-instructions",
               "--disable-builtin-mcps", "--no-auto-update", "--allow-tool=write", "--deny-tool=url",
               "--available-tools=" + COPILOT_FILE_TOOLS + (",bash" if checker else "")]
        cmd += ["--allow-tool=shell(python3:*)"] if checker else []
        return cmd + (["--model", model] if model else [])
    raise SystemExit(f"unknown agent {agent}")


def clean_env(agent):
    """The parent environment may belong to another Claude Code session; its
    CLAUDE*/ANTHROPIC* variables (proxy URL, effort, session ids) must not leak
    into the agent under test."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CLAUDE", "ANTHROPIC"))}
    if agent == "copilot":
        if not (COPILOT_HOME / "config.json").exists():
            raise SystemExit(f"create {COPILOT_HOME}/config.json with the login entries of ~/.copilot/config.json")
        env["COPILOT_HOME"] = str(COPILOT_HOME)
    return env


def usage(agent, out):
    """Model(s) that answered, cost in USD (claude) and premium requests (copilot)."""
    models, cost, premium = set(), None, None
    for line in out.splitlines():
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if not isinstance(d, dict):
            continue
        if agent == "claude" and d.get("type") == "result":
            models |= set(d.get("modelUsage") or {})
            cost = d.get("total_cost_usd")
        elif agent == "copilot":
            data = d.get("data") or {}
            if data.get("model"):
                models.add(data["model"])
            if d.get("type") == "session.usage_checkpoint":
                premium = data.get("totalPremiumRequests")
        elif agent == "codex" and d.get("type") == "turn.completed":
            cost = d.get("usage")
    return {"models": sorted(models), "cost_usd" if agent == "claude" else "usage": cost} | (
        {"premium_requests": premium} if agent == "copilot" else {})


def version(agent):
    if agent == "copy-human":
        return "test"
    try:
        return subprocess.run([agent, "--version"], capture_output=True, text=True, timeout=60).stdout.strip().splitlines()[0]
    except (OSError, subprocess.SubprocessError) as e:
        return f"unknown ({e})"


SUSPECT = ("human.patch", "after.c", "after.cpp", "after.h", "after.hpp", "clang-tidy-fix-bench",
           "github.com/", "cache/repos")


def audit(log, checker):
    """Reasons a transcript looks like the agent left the task's sandbox."""
    t = log.lower()
    why = [s for s in SUSPECT if s in t]
    if not checker and "clang-tidy " in t and "--checks" in t:
        why.append("ran clang-tidy")
    return why


def one(task, a, cfg, ver, logs):
    sid = task.name
    m = json.loads((SAMPLES / sid / "meta.json").read_text())
    local = Path(m["file"]).name
    with tempfile.TemporaryDirectory(prefix="ctfb-") as tmp:
        ws = Path(tmp) / "task"
        shutil.copytree(task, ws)
        t0 = time.time()
        if a.agent == "copy-human":
            shutil.copy(SAMPLES / sid / f"after{Path(m['file']).suffix}", ws / local)
            (ws / "NOTE.md").write_text("copied the human fix (test agent)\n")
            rc, out, err = 0, "", ""
        else:
            env = clean_env(a.agent)
            try:
                p = subprocess.run(command(a.agent, a.model, ws, cfg["checker"]), cwd=ws, env=env,
                                   capture_output=True, text=True, timeout=a.timeout, stdin=subprocess.DEVNULL)
                rc, out, err = p.returncode, p.stdout, p.stderr
            except subprocess.TimeoutExpired as e:
                rc, out, err = "timeout", e.stdout or "", e.stderr or ""
                out = out.decode() if isinstance(out, bytes) else out
                err = err.decode() if isinstance(err, bytes) else err
        secs = round(time.time() - t0, 1)
        shutil.copy(ws / local, task / local)
        if (ws / "NOTE.md").exists():
            shutil.copy(ws / "NOTE.md", task / "NOTE.md")
    log = f"$ {a.agent} (rc={rc}, {secs}s)\n--- stdout\n{out}\n--- stderr\n{err}\n"
    (logs / f"{sid}.log").write_text(log)
    rec = {"id": sid, "agent": a.agent, "model": a.model, "cli_version": ver, "rc": rc, "seconds": secs,
           "audit": audit(log, cfg["checker"])} | usage(a.agent, out)
    (task / ".done").write_text(json.dumps(rec) + "\n")
    print(sid, rc, secs, rec["audit"] or "", flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--agent", required=True, choices=("claude", "codex", "copilot", "copy-human"))
    ap.add_argument("--model")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--limit", type=int)
    a = ap.parse_args()
    rd = ROOT / "runs" / a.run
    cfg = json.loads((rd / "config.json").read_text())
    logs = rd / "logs"
    logs.mkdir(exist_ok=True)
    todo = [t for t in sorted((rd / "tasks").iterdir()) if not (t / ".done").exists()][: a.limit]
    ver = version(a.agent)
    print(f"{len(todo)} tasks, agent {a.agent} {ver}, model {a.model or 'default'}")
    with ThreadPoolExecutor(a.jobs) as ex:
        recs = list(ex.map(lambda t: one(t, a, cfg, ver, logs), todo))
    with (rd / "agent.jsonl").open("a") as f:
        f.writelines(json.dumps(r) + "\n" for r in recs)
    flagged = [r["id"] for r in recs if r["audit"]]
    if flagged:
        print(f"{len(flagged)} transcripts flagged by audit(); read them before scoring:", *flagged, sep="\n  ")


if __name__ == "__main__":
    main()
