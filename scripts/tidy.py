"""Shared helpers: sparse repo checkouts and best-effort clang-tidy runs.

Real projects need their own build flags. We do not build them; instead we
check out headers, add every header directory to the include path, and
comment out #include lines whose target cannot be found (line numbers stay the
same). That is enough for most local checks to see the code they flag.
"""
import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPOS = ROOT / "cache" / "repos"
HDR = ("*.h", "*.hh", "*.hpp", "*.hxx", "*.inl", "*.ipp", "*.tcc", "*.def", "*.inc")
SDK = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
RES = subprocess.run(["xcrun", "clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
CLANG_TIDY = os.environ.get("CLANG_TIDY", "clang-tidy")

DIAG = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): (?P<sev>warning|error): (?P<msg>.*?) \[(?P<check>[^\]]+)\]$")
MISSING = re.compile(r"'(?P<inc>[^']+)' file not found")


def git(repo_dir, *args, check=True):
    return subprocess.run(["git", "-C", str(repo_dir), *args], capture_output=True, text=True, check=check)


def checkout(repo, sha, extra_files):
    """Fetch `sha` (depth 2, so its parent too) and check out headers + extra_files."""
    d = REPOS / repo.replace("/", "__")
    if not (d / ".git").exists():
        d.mkdir(parents=True, exist_ok=True)
        git(d, "init", "-q")
        git(d, "remote", "add", "origin", f"https://github.com/{repo}.git")
        git(d, "config", "core.sparseCheckout", "true")
    patterns = list(HDR) + [".clang-tidy"] + ["/" + f for f in extra_files]
    (d / ".git" / "info").mkdir(exist_ok=True)
    (d / ".git" / "info" / "sparse-checkout").write_text("\n".join(patterns) + "\n")
    have = git(d, "cat-file", "-e", sha + "^{commit}", check=False).returncode == 0
    if not have:
        git(d, "fetch", "-q", "--depth=2", "--filter=blob:none", "origin", sha)
    return d


def switch(d, sha):
    git(d, "reset", "-q", "--hard")
    git(d, "clean", "-qfdx")
    git(d, "checkout", "-q", "-f", sha)
    git(d, "read-tree", "-mu", "HEAD")  # apply sparse patterns


def include_dirs(d, limit=600):
    dirs = set()
    for dp, dn, fn in os.walk(d):
        if ".git" in dp.split(os.sep):
            continue
        if any(f.endswith(tuple(h[1:] for h in HDR)) for f in fn):
            p = Path(dp)
            while True:
                dirs.add(p)
                if p == d:
                    break
                p = p.parent
    dirs = sorted(dirs, key=lambda p: (len(p.parts), str(p)))[:limit]  # deterministic cut
    return [str(p) for p in dirs]


def lang_flags(path, repo_is_cpp):
    if path.endswith(".c") or (path.endswith(".h") and not repo_is_cpp):
        return ["-x", "c", "-std=gnu11"]
    return ["-x", "c++", "-std=gnu++20"]


def stub(text, missing):
    """Comment out #include lines naming any header in `missing`; keep line count."""
    out = []
    for line in text.split("\n"):
        m = re.match(r'\s*#\s*include\s*[<"]([^>"]+)[>"]', line)
        if m and m.group(1) in missing:
            line = "// [stubbed] " + line
        out.append(line)
    return "\n".join(out)


def run_tidy(d, rel, check, flags, incs):
    cmd = [CLANG_TIDY, str(d / rel), f"--checks=-*,{check}", "--warnings-as-errors=-*", "--quiet",
           "--", *flags, "-isysroot", SDK, "-resource-dir", RES, "-ferror-limit=0", "-w"]
    cmd += [f"-I{i}" for i in incs]
    p = subprocess.run(cmd, capture_output=True, text=True, cwd=d, timeout=300)
    diags = []
    for line in (p.stdout + "\n" + p.stderr).splitlines():
        m = DIAG.match(line)
        if m:
            g = m.groupdict()
            g["line"] = int(g["line"])
            g["col"] = int(g["col"])
            diags.append(g)
    return diags


def analyze(d, rel, content, check, flags, missing, rounds=30):
    """Write `content` to d/rel, stub unresolved includes (anywhere in the repo),
    run clang-tidy and return (target-check diagnostics in rel, error count, missing).
    `missing` is updated in place with any further headers that can't be found."""
    target = d / rel
    orig = {}
    incs = include_dirs(d)
    try:
        for _ in range(rounds):
            target.write_text(stub(content, missing))
            for f, t in orig.items():
                f.write_text(stub(t, missing))
            diags = run_tidy(d, rel, check, flags, incs)
            new = False
            for g in diags:
                if g["sev"] != "error":
                    continue
                m = MISSING.search(g["msg"])
                if not m:
                    continue
                inc = m.group("inc")
                fpath = Path(g["file"])
                inside = str(fpath).startswith(str(d))
                if not inside:
                    continue
                # `missing` may be pre-seeded (check_fix.py), so a known-missing
                # header can still need stubbing in another file of the repo.
                other = fpath.resolve() != target.resolve() and fpath not in orig
                if inc not in missing or other:
                    missing.add(inc)
                    new = True
                    if other:
                        orig[fpath] = fpath.read_text(errors="replace")
            if not new:
                break
        mine = [g for g in diags if Path(g["file"]).resolve() == target.resolve()]
        warns = [g for g in mine if g["sev"] == "warning" and check in g["check"].split(",")]
        errors = [g for g in mine if g["sev"] == "error"]
        return warns, len(errors)
    finally:
        for f, t in orig.items():
            f.write_text(t)


def keyed(warns, text):
    """Key warnings by (message, stripped source line) so they survive line shifts."""
    lines = text.split("\n")
    out = []
    for w in warns:
        src = lines[w["line"] - 1].strip() if 0 < w["line"] <= len(lines) else ""
        out.append(((w["msg"], src), w))
    return out


def diff_keys(before, after):
    """Multiset difference: (disappeared, appeared)."""
    from collections import Counter
    a = Counter(k for k, _ in after)
    b = Counter(k for k, _ in before)
    gone, new = [], []
    used = Counter()
    for k, w in before:
        used[k] += 1
        if used[k] > a[k]:
            gone.append(w)
    used = Counter()
    for k, w in after:
        used[k] += 1
        if used[k] > b[k]:
            new.append(w)
    return gone, new
