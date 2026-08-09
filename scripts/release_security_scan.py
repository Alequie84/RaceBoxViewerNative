#!/usr/bin/env python3
"""Fail a public release when source or an archive contains credential material."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path


PATTERNS = {
    "known personal endpoint": re.compile(rb"100\.73\.60\.87"),
    "personal Linux path": re.compile(rb"/home/alex(?:/|\\)"),
    "personal Windows path": re.compile(rb"C:\\Users\\aalex(?:\\|/)", re.IGNORECASE),
    "private key": re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "OpenAI-style key": re.compile(rb"\bsk-(?:proj-)?[A-Za-z0-9_-]{20,}"),
    "GitHub token": re.compile(rb"\b(?:ghp|github_pat)_[A-Za-z0-9_]{20,}"),
    "literal bearer token": re.compile(rb"Bearer[ \t]+[A-Za-z0-9_-]{24,}"),
}
MAX_ENTRY_BYTES = 96 * 1024 * 1024


def findings(label: str, content: bytes) -> list[str]:
    return [f"{label}: {name}" for name, pattern in PATTERNS.items() if pattern.search(content)]


def scan_file(path: Path) -> list[str]:
    problems: list[str] = []
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            for item in archive.infolist():
                if item.is_dir() or item.file_size > MAX_ENTRY_BYTES:
                    continue
                problems.extend(findings(f"{path}!{item.filename}", archive.read(item)))
    elif path.stat().st_size <= MAX_ENTRY_BYTES:
        problems.extend(findings(str(path), path.read_bytes()))
    return problems


def source_files() -> list[Path]:
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True,
        capture_output=True,
    )
    return [Path(item.decode("utf-8")) for item in result.stdout.split(b"\0") if item]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--source", action="store_true")
    parser.add_argument("--git-diff", action="store_true")
    args = parser.parse_args()
    problems: list[str] = []
    candidates = list(args.paths)
    if args.source:
        candidates.extend(source_files())
    for path in dict.fromkeys(candidates):
        if path.is_file():
            problems.extend(scan_file(path))
    if args.git_diff:
        raw_diff = subprocess.run(
            ["git", "diff", "--no-ext-diff", "--unified=0", "HEAD"],
            check=True,
            capture_output=True,
        ).stdout
        # Removed private defaults are expected in this release. Scan only new
        # text, which is what can introduce a public exposure.
        additions = b"\n".join(
            line[1:] for line in raw_diff.splitlines()
            if line.startswith(b"+") and not line.startswith(b"+++")
        )
        problems.extend(findings("git diff additions", additions))
    if problems:
        print("PUBLIC RELEASE SECURITY SCAN FAILED", file=sys.stderr)
        for problem in problems:
            print(f"- {problem}", file=sys.stderr)
        return 1
    print(f"Public release security scan passed ({len(set(candidates))} file paths checked)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
