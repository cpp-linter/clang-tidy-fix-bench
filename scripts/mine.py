#!/usr/bin/env python3
"""Mine candidate "clang-tidy warning -> merged fix" commits from GitHub.

For each target check, search commits whose message names the check, keep the
ones in permissively licensed, non-fork repos that change a few C/C++ files,
and write them to data/candidates.jsonl. Validation happens in validate.py.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / "cache"
OUT = ROOT / "data" / "candidates.jsonl"

# check -> names that may appear in commit messages (aliases included)
CHECKS = {
    "bugprone-narrowing-conversions": ["bugprone-narrowing-conversions", "cppcoreguidelines-narrowing-conversions"],
    "bugprone-branch-clone": ["bugprone-branch-clone"],
    "bugprone-unused-return-value": ["bugprone-unused-return-value", "cert-err33-c"],
    "cppcoreguidelines-special-member-functions": ["cppcoreguidelines-special-member-functions", "hicpp-special-member-functions"],
    "readability-magic-numbers": ["readability-magic-numbers", "cppcoreguidelines-avoid-magic-numbers"],
    "bugprone-use-after-move": ["bugprone-use-after-move", "hicpp-invalid-access-moved"],
    "bugprone-too-small-loop-variable": ["bugprone-too-small-loop-variable"],
    "bugprone-signed-char-misuse": ["bugprone-signed-char-misuse", "cert-str34-c"],
    "bugprone-unchecked-optional-access": ["bugprone-unchecked-optional-access"],
    "bugprone-implicit-widening-of-multiplication-result": ["bugprone-implicit-widening-of-multiplication-result"],
}

PERMISSIVE = {"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "BSL-1.0", "Zlib", "Unlicense", "0BSD", "MIT-0", "BSD-3-Clause-Clear", "NCSA"}
SRC_EXT = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl")


def gh(*args, retries=4):
    for i in range(retries):
        p = subprocess.run(["gh", "api", *args], capture_output=True, text=True)
        if p.returncode == 0:
            return json.loads(p.stdout) if p.stdout.strip() else None
        if "rate limit" in p.stderr.lower() or "secondary" in p.stderr.lower():
            time.sleep(30 * (i + 1))
            continue
        return None
    return None


def cached(name, fn):
    f = CACHE / name
    if f.exists():
        return json.loads(f.read_text())
    v = fn()
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(json.dumps(v))
    return v


def license_of(repo):
    def get():
        r = gh(f"repos/{repo}")
        if not r:
            return None
        return {"spdx": (r.get("license") or {}).get("spdx_id"), "fork": r.get("fork"),
                "stars": r.get("stargazers_count"), "archived": r.get("archived")}
    return cached("repo/" + repo.replace("/", "__") + ".json", get)


def search(name, page):
    def get():
        time.sleep(2.2)  # search API: 30 req/min
        r = gh("-X", "GET", "search/commits", "-f", f'q="{name}" -repo:llvm/llvm-project',
               "-f", "per_page=100", "-f", f"page={page}")
        return (r or {}).get("items", [])
    return cached(f"search/{name}-p{page}.json", get)


def commit(repo, sha):
    return cached(f"commit/{repo.replace('/', '__')}__{sha}.json", lambda: gh(f"repos/{repo}/commits/{sha}"))


def main():
    pages = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    seen = set()
    if OUT.exists():
        for line in OUT.read_text().splitlines():
            seen.add(json.loads(line)["sha"])
    out = OUT.open("a")
    for check, names in CHECKS.items():
        kept = 0
        for name in names:
            for page in range(1, pages + 1):
                items = search(name, page)
                if not items:
                    break
                for it in items:
                    sha = it["sha"]
                    repo = it["repository"]["full_name"]
                    if sha in seen or "llvm" in repo.lower() or it["repository"].get("fork"):
                        continue
                    seen.add(sha)
                    if len(it.get("parents", [])) != 1:
                        continue
                    lic = license_of(repo)
                    if not lic or lic["spdx"] not in PERMISSIVE:
                        continue
                    c = commit(repo, sha)
                    if not c:
                        continue
                    files = c.get("files", [])
                    src = [f for f in files if f["filename"].endswith(SRC_EXT) and f["status"] == "modified"]
                    changes = sum(f["changes"] for f in src)
                    if not src or len(src) > 8 or changes > 200 or changes == 0:
                        continue
                    rec = {
                        "check": check, "matched_name": name, "repo": repo, "sha": sha,
                        "parent": c["parents"][0]["sha"], "license": lic["spdx"], "stars": lic["stars"],
                        "message": c["commit"]["message"].split("\n")[0][:200],
                        "files": [f["filename"] for f in src], "changes": changes,
                        "url": c["html_url"],
                    }
                    out.write(json.dumps(rec) + "\n")
                    out.flush()
                    kept += 1
        print(f"{check}: {kept} candidates", flush=True)


if __name__ == "__main__":
    main()
