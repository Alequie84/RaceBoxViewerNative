#!/usr/bin/env python3
"""Load a private RaceBox env file, then start the gateway."""

from __future__ import annotations

import argparse
import os
import runpy
from pathlib import Path


def load_env(path: Path) -> None:
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if not key or not key.replace("_", "").isalnum():
            raise ValueError(f"Invalid environment key in {path}")
        os.environ[key] = value.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", required=True, type=Path)
    parser.add_argument("--app-dir", required=True, type=Path)
    args = parser.parse_args()
    load_env(args.env.expanduser().resolve())
    app_dir = args.app_dir.expanduser().resolve()
    os.chdir(app_dir)
    runpy.run_path(str(app_dir / "gateway.py"), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
