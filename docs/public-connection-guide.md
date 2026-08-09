# Public OpenClaw connection guide

## What the gateway is for

OpenClaw's Chat Completions endpoint is disabled by default, and its bearer
credential controls the full OpenClaw gateway. RaceBox therefore keeps a narrow
gateway between the apps and OpenClaw. Only that gateway reads the local
OpenClaw credential. Windows and Android receive a separate RaceBox client or
session token.

## Requirements

- Existing OpenClaw installation with a working provider/model
- Python 3.11+
- Existing Tailscale installation for private HTTP, or your own HTTPS proxy
- Windows PowerShell 5.1+ or Bash on the gateway host

The helpers do not install these prerequisites.

The generated RaceBox client token is written only to the private `gateway.env`
path reported by setup. Copy that one client token into the Windows connection
panel when you want authenticated live access. Do not share the file or the
OpenClaw operator token.

## Windows host

```powershell
.\setup-gateway.ps1 -TailscaleIp <your-tailscale-ip> -DryRun
.\setup-gateway.ps1 -TailscaleIp <your-tailscale-ip> -InstallStartup
```

Without `-InstallStartup`, the helper prints a manual start command. With it, a
Task Scheduler at-logon entry is created. Pass `-Model <provider/model>` to set
the new agent's model during creation.

## Linux host

```bash
bash ./setup-gateway.sh --tailscale-ip <your-tailscale-ip> --dry-run
bash ./setup-gateway.sh --tailscale-ip <your-tailscale-ip> --install-startup
```

With `--install-startup`, a systemd user service is enabled. This does not
install a Linux Viewer; Linux is only a supported gateway host.

## Rollback

Each successful setup prints a private rollback manifest path. Keep that path
on the gateway host, then run:

```powershell
.\setup-gateway.ps1 -Rollback <manifest-path>
```

or:

```bash
bash ./setup-gateway.sh --rollback <manifest-path>
```

Rollback stops/removes the optional startup entry, restores overwritten files
and the pre-change OpenClaw configuration, and removes files created by setup.

## Health contract

Authenticate with the generated RaceBox client token and request `/health`.
This release reports:

- `contract`: `racebox-crew-chief-v14`
- `connection_contract`: `racebox-crew-chief-connection-v1`
- `prompt_revision`: `racebox-crew-chief-behavior-v14-public-1`
- `companion_contract`: `racebox-companion-session-v1`

Never paste the health token into an issue, log, screenshot, or repository file.
