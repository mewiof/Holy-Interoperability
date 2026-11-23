#!/usr/bin/env python3
"""Repository hygiene check.

Fails if the working tree contains something that must never be committed:

  * binary files, or files with extensions that only foreign material has
    (executables, libraries, console package containers, archives, dumps);
  * oversized files;
  * paths into a user profile (they leak who and where the developer is);
  * any term from the developer's private deny-list.

The deny-list lives in local/denylist.txt, which is git-ignored: one term per
line, matched case-insensitively as a substring; blank lines and lines
starting with '#' are ignored. Put the names of whatever you test with there
(titles, publishers, ids, file names). The check never prints a matched term,
only its line number in the deny-list, so its output is safe to share.

Usage:
  python tools/hygiene/check.py            check tracked and untracked files
  python tools/hygiene/check.py --commits  also check commit messages
"""

import os
import re
import subprocess
import sys

FORBIDDEN_EXTENSIONS = {
    ".exe", ".dll", ".sys", ".lib", ".pdb", ".obj", ".o", ".a", ".so",
    ".xvd", ".xvc", ".xvi", ".msixvc", ".bin", ".dmp", ".log",
    ".zip", ".7z", ".rar", ".iso", ".img",
}
MAX_FILE_BYTES = 512 * 1024
PROFILE_PATH = re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s<>%$\"']+", re.IGNORECASE)


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def load_denylist(root):
    path = os.path.join(root, "local", "denylist.txt")
    terms = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as handle:
            for number, line in enumerate(handle, start=1):
                term = line.strip()
                if term and not term.startswith("#"):
                    terms.append((number, term.lower()))
    return terms


def denied(text, terms):
    lowered = text.lower()
    return [number for number, term in terms if term in lowered]


def main():
    root = git("rev-parse", "--show-toplevel").decode().strip()
    os.chdir(root)
    terms = load_denylist(root)
    problems = []

    listing = git("ls-files", "--cached", "--others", "--exclude-standard", "-z").decode()
    files = [name for name in listing.split("\0") if name]
    for name in files:
        if not os.path.isfile(name):
            continue  # deleted in the working tree
        for number in denied(name, terms):
            problems.append(f"{name}: file name contains deny-list term #{number}")
        extension = os.path.splitext(name)[1].lower()
        if extension in FORBIDDEN_EXTENSIONS:
            problems.append(f"{name}: files of type '{extension}' must not be committed")
            continue
        size = os.path.getsize(name)
        if size > MAX_FILE_BYTES:
            problems.append(f"{name}: {size} bytes is larger than the {MAX_FILE_BYTES} byte limit")
            continue
        with open(name, "rb") as handle:
            data = handle.read()
        if b"\0" in data:
            problems.append(f"{name}: binary file")
            continue
        text = data.decode("utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), start=1):
            if PROFILE_PATH.search(line):
                problems.append(f"{name}:{line_number}: path into a user profile")
            for number in denied(line, terms):
                problems.append(f"{name}:{line_number}: contains deny-list term #{number}")

    if "--commits" in sys.argv[1:]:
        log = git("log", "--format=%H%x00%B%x01").decode("utf-8", errors="replace")
        for entry in log.split("\x01"):
            if "\0" not in entry:
                continue
            commit, message = entry.strip().split("\0", 1)
            if PROFILE_PATH.search(message):
                problems.append(f"commit {commit[:12]}: message contains a path into a user profile")
            for number in denied(message, terms):
                problems.append(f"commit {commit[:12]}: message contains deny-list term #{number}")

    for problem in problems:
        print(problem)
    scope = f"{len(files)} files" + (", commit messages" if "--commits" in sys.argv[1:] else "")
    if not terms:
        print("note: no local/denylist.txt found; title names cannot be checked on this machine")
    if problems:
        print(f"hygiene check FAILED: {len(problems)} problem(s) in {scope}")
        return 1
    print(f"hygiene check passed ({scope}, {len(terms)} deny-list terms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
