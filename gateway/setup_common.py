#!/usr/bin/env python3
"""Shared Windows/Linux setup logic for a user-owned RaceBox OpenClaw gateway."""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from typing import Any


AGENT_ID = "racebox-crew-chief"
PROMPT_REVISION = "racebox-crew-chief-behavior-v14-public-1"
CONTRACT = "racebox-crew-chief-v14"
PACKAGE_FILES = (
    "gateway.py",
    "companion_store.py",
    "intelligence_router.py",
    "telemetry_analytics.py",
    "benchmark_lanes.py",
    "compare_single_agent.py",
    "racebox-vehicle-dynamics.SKILL.md",
    "racebox-agent-instructions.md",
    "run_gateway.py",
)
TOOL_DENY = [
    "group:runtime",
    "group:fs",
    "group:sessions",
    "group:memory",
    "group:web",
    "group:ui",
    "group:automation",
    "group:messaging",
    "group:nodes",
    "group:agents",
    "group:media",
    "group:plugins",
]


def validate_tailscale_address(value: str) -> str:
    address = ipaddress.ip_address(value.strip())
    network = ipaddress.ip_network("100.64.0.0/10")
    if not isinstance(address, ipaddress.IPv4Address) or address not in network:
        raise ValueError("RaceBox HTTP gateway address must be a Tailscale IPv4 address in 100.64.0.0/10")
    return str(address)


def detect_tailscale_address() -> str:
    result = subprocess.run(
        ["tailscale", "ip", "-4"], check=True, capture_output=True, text=True
    )
    candidates = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(candidates) != 1:
        raise RuntimeError("Could not detect exactly one Tailscale IPv4 address; pass --tailscale-ip")
    return validate_tailscale_address(candidates[0])


def platform_paths(host: str, install_root: Path | None) -> dict[str, Path]:
    if install_root:
        root = install_root.expanduser().resolve()
        return {
            "app": root / "app",
            "config": root / "config",
            "state": root / "state",
            "workspace": root / "openclaw-workspace",
            "service": root / "startup",
        }
    home = Path.home()
    if host == "windows":
        local = Path(os.environ.get("LOCALAPPDATA", home / "AppData" / "Local"))
        root = local / "RaceBoxCrewChiefGateway"
        return {
            "app": root / "app",
            "config": root / "config",
            "state": root / "state",
            "workspace": root / "openclaw-workspace",
            "service": root / "startup",
        }
    return {
        "app": home / ".local" / "share" / "racebox-crew-chief-gateway",
        "config": home / ".config" / "racebox-crew-chief-gateway",
        "state": home / ".local" / "state" / "racebox-crew-chief-gateway",
        "workspace": home / ".local" / "share" / "racebox-crew-chief-workspace",
        "service": home / ".config" / "systemd" / "user",
    }


def openclaw_config_path() -> Path:
    supplied = os.environ.get("OPENCLAW_CONFIG", "").strip()
    return Path(supplied).expanduser() if supplied else Path.home() / ".openclaw" / "openclaw.json"


def run(command: list[str], *, dry_run: bool = False) -> subprocess.CompletedProcess[str] | None:
    if dry_run:
        print("DRY RUN:", " ".join(command))
        return None
    return subprocess.run(command, check=True, text=True, capture_output=True)


def copy_with_backup(source: Path, destination: Path, manifest: dict[str, Any]) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    backup_root = Path(manifest["backup_root"])
    if destination.exists():
        backup = backup_root / "files" / str(len(manifest["replaced"]))
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(destination, backup)
        manifest["replaced"].append({"path": str(destination), "backup": str(backup)})
    else:
        manifest["created"].append(str(destination))
    shutil.copy2(source, destination)


def write_with_backup(destination: Path, content: str, manifest: dict[str, Any]) -> None:
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False, newline="\n") as handle:
        handle.write(content)
        temporary = Path(handle.name)
    try:
        copy_with_backup(temporary, destination, manifest)
    finally:
        temporary.unlink(missing_ok=True)


def configure_openclaw(config_path: Path, workspace: Path, model: str | None = None) -> None:
    data = json.loads(config_path.read_text(encoding="utf-8"))
    agents = data.setdefault("agents", {})
    entries = agents.setdefault("list", [])
    agent = next((item for item in entries if item.get("id") == AGENT_ID), None)
    if agent is None:
        raise RuntimeError("openclaw agents add did not create the RaceBox agent")
    agent.update(
        {
            "name": "RaceBox Crew Chief",
            "workspace": str(workspace),
            "skills": ["racebox-vehicle-dynamics"],
            "heartbeat": {"every": "0m"},
            "tools": {"profile": "minimal", "deny": TOOL_DENY},
            "thinkingDefault": "xhigh",
        }
    )
    if model:
        agent["model"] = model
    temporary = config_path.with_suffix(config_path.suffix + ".racebox.tmp")
    temporary.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, config_path)


def env_text(address: str, paths: dict[str, Path], token: str, config_path: Path) -> str:
    return "\n".join(
        [
            f"CREW_CHIEF_BIND_HOST={address}",
            "CREW_CHIEF_BIND_PORT=18804",
            f"CREW_CHIEF_PUBLIC_URL=http://{address}:18804",
            f"CREW_CHIEF_CLIENT_TOKEN={token}",
            "OPENCLAW_TARGET=http://127.0.0.1:18789",
            f"OPENCLAW_MODEL=openclaw/{AGENT_ID}",
            f"OPENCLAW_CONFIG={config_path}",
            f"CREW_CHIEF_STATE_DIRECTORY={paths['state']}",
            "ALLOWED_CLIENT_CIDRS=127.0.0.0/8,100.64.0.0/10",
            "MAX_BODY_BYTES=25165824",
            "OPENCLAW_TIMEOUT_SECONDS=240",
            "CREW_CHIEF_VISION_ENABLED=0",
            "",
        ]
    )


def linux_service(paths: dict[str, Path], python: str) -> str:
    return f"""[Unit]
Description=RaceBox Crew Chief gateway
After=network-online.target

[Service]
Type=simple
ExecStart={python} {paths['app'] / 'run_gateway.py'} --env {paths['config'] / 'gateway.env'} --app-dir {paths['app']}
WorkingDirectory={paths['app']}
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
"""


def validate_health(address: str, token: str) -> None:
    request = urllib.request.Request(
        f"http://{address}:18804/health",
        headers={"Authorization": f"Bearer {token}"},
    )
    with urllib.request.urlopen(request, timeout=10) as response:
        value = json.loads(response.read().decode("utf-8"))
    if value.get("contract") != CONTRACT or value.get("prompt_revision") != PROMPT_REVISION:
        raise RuntimeError("Gateway health contract or prompt revision did not match this release")


def validate_temporary_gateway(paths: dict[str, Path], env_file: Path, address: str, token: str) -> None:
    process = subprocess.Popen(
        [
            sys.executable,
            str(paths["app"] / "run_gateway.py"),
            "--env",
            str(env_file),
            "--app-dir",
            str(paths["app"]),
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        last_error: Exception | None = None
        for _ in range(20):
            if process.poll() is not None:
                raise RuntimeError("The installed gateway stopped before its health check")
            try:
                validate_health(address, token)
                return
            except Exception as error:  # The listener may still be starting.
                last_error = error
                time.sleep(0.25)
        raise RuntimeError(f"Installed gateway health check failed: {last_error}")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()


def rollback(manifest_path: Path) -> int:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("startup") and manifest.get("host") == "windows" and shutil.which("schtasks"):
        subprocess.run(
            ["schtasks", "/Delete", "/TN", "RaceBox Crew Chief Gateway", "/F"],
            check=False,
            capture_output=True,
        )
    elif manifest.get("startup") and manifest.get("host") == "linux" and shutil.which("systemctl"):
        subprocess.run(
            ["systemctl", "--user", "disable", "--now", "racebox-crew-chief-gateway.service"],
            check=False,
            capture_output=True,
        )
    for item in reversed(manifest.get("replaced", [])):
        shutil.copy2(item["backup"], item["path"])
    for raw_path in reversed(manifest.get("created", [])):
        path = Path(raw_path)
        if path.is_file():
            path.unlink()
    if manifest.get("host") == "linux" and shutil.which("systemctl"):
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=False, capture_output=True)
    print(f"Rollback completed from {manifest_path}")
    return 0


def install(args: argparse.Namespace) -> int:
    source = Path(__file__).resolve().parent
    paths = platform_paths(args.host, args.install_root)
    address = validate_tailscale_address(args.tailscale_ip) if args.tailscale_ip else detect_tailscale_address()
    config_path = openclaw_config_path()
    if args.dry_run:
        print(f"Host: {args.host}; Tailscale: {address}; app: {paths['app']}")
        run(["openclaw", "agents", "add", AGENT_ID, "--workspace", str(paths["workspace"]), "--non-interactive", "--json"], dry_run=True)
        run(["openclaw", "config", "set", "gateway.http.endpoints.chatCompletions.enabled", "true"], dry_run=True)
        print("DRY RUN: generate a separate RaceBox client token, install files, validate /health")
        return 0
    for executable in (sys.executable, "openclaw", "tailscale"):
        if executable != sys.executable and shutil.which(executable) is None:
            raise RuntimeError(f"Required existing tool was not found: {executable}")
    if not config_path.is_file():
        raise RuntimeError(f"OpenClaw config was not found: {config_path}")
    timestamp = time.strftime("%Y%m%d-%H%M%S")
    backup_root = paths["config"] / "backups" / timestamp
    manifest: dict[str, Any] = {
        "backup_root": str(backup_root), "replaced": [], "created": [],
        "startup": args.install_startup, "host": args.host
    }
    backup_root.mkdir(parents=True, exist_ok=True)
    for directory in paths.values():
        directory.mkdir(parents=True, exist_ok=True)
    for name in PACKAGE_FILES:
        copy_with_backup(source / name, paths["app"] / name, manifest)
    skill = paths["workspace"] / "skills" / "racebox-vehicle-dynamics" / "SKILL.md"
    copy_with_backup(source / "racebox-vehicle-dynamics.SKILL.md", skill, manifest)
    copy_with_backup(source / "racebox-agent-instructions.md", paths["workspace"] / "AGENTS.md", manifest)
    openclaw_backup = backup_root / "openclaw.json.before"
    shutil.copy2(config_path, openclaw_backup)
    manifest["replaced"].append({"path": str(config_path), "backup": str(openclaw_backup)})
    listed = run(["openclaw", "agents", "list", "--json"])
    existing = listed.stdout if listed else ""
    if f'"{AGENT_ID}"' not in existing:
        command = ["openclaw", "agents", "add", AGENT_ID, "--workspace", str(paths["workspace"]), "--non-interactive", "--json"]
        if args.model:
            command[4:4] = ["--model", args.model]
        run(command)
    configure_openclaw(config_path, paths["workspace"], args.model)
    run(["openclaw", "config", "set", "gateway.http.endpoints.chatCompletions.enabled", "true"])
    client_token = secrets.token_urlsafe(32)
    env_file = paths["config"] / "gateway.env"
    write_with_backup(env_file, env_text(address, paths, client_token, config_path), manifest)
    try:
        os.chmod(env_file, 0o600)
    except OSError:
        pass
    if args.install_startup and args.host == "linux":
        service_file = paths["service"] / "racebox-crew-chief-gateway.service"
        write_with_backup(service_file, linux_service(paths, sys.executable), manifest)
        run(["systemctl", "--user", "daemon-reload"])
        run(["systemctl", "--user", "enable", "--now", service_file.name])
        time.sleep(1)
        validate_health(address, client_token)
    elif args.install_startup and args.host == "windows":
        action = f'"{sys.executable}" "{paths["app"] / "run_gateway.py"}" --env "{env_file}" --app-dir "{paths["app"]}"'
        run(["schtasks", "/Create", "/TN", "RaceBox Crew Chief Gateway", "/SC", "ONLOGON", "/TR", action, "/F"])
        run(["schtasks", "/Run", "/TN", "RaceBox Crew Chief Gateway"])
        time.sleep(1)
        validate_health(address, client_token)
    else:
        validate_temporary_gateway(paths, env_file, address, client_token)
    manifest_path = backup_root / "rollback.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"RaceBox gateway configured. Private client token stored in {env_file}")
    print(f"Viewer/phone gateway URL: http://{address}:18804")
    print(f"Rollback manifest: {manifest_path}")
    if not args.install_startup:
        print(f"Start with: {sys.executable} {paths['app'] / 'run_gateway.py'} --env {env_file} --app-dir {paths['app']}")
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    result.add_argument("--host", choices=("windows", "linux"), required=True)
    result.add_argument("--tailscale-ip")
    result.add_argument("--model", help="Optional OpenClaw model ID for the new RaceBox agent")
    result.add_argument("--install-root", type=Path, help="Override paths (mainly for testing)")
    result.add_argument("--install-startup", action="store_true")
    result.add_argument("--dry-run", action="store_true")
    result.add_argument("--rollback", type=Path)
    return result


def main() -> int:
    args = parser().parse_args()
    if args.rollback:
        return rollback(args.rollback.expanduser().resolve())
    return install(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Setup failed: {error}", file=sys.stderr)
        raise SystemExit(1)
