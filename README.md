# RaceBox Telemetry Viewer + Crew Chief

RaceBox turns RC-car telemetry into a native Windows analysis workspace and an
optional phone conversation with an evidence-constrained Crew Chief. The public
release starts safely in an **Offline demo**: it loads the included Richmond
fixture, uses scripted Crew Chief answers, and makes no network calls.

Release versions:

- Windows Viewer: `2.0.0.020`
- Android Crew Chief companion: `1.4.0` (`versionCode 5`)
- Gateway contract: `racebox-crew-chief-v14`
- Connection settings: `racebox-crew-chief-connection-v1`
- Prompt revision: `racebox-crew-chief-behavior-v14-public-1`

## Try the offline demo

### Windows

1. Download `RaceBoxTelemetryViewer-2.0.0.020-win64.zip` from Releases.
2. Extract the whole ZIP and run `Start RaceBox Demo.cmd`.
3. Open **Crew Chief > Connection**, leave **Offline demo** selected, and ask a
   question about the included Richmond recording.

The ZIP is portable. Windows may show a SmartScreen warning because this
hackathon build is not code-signed.

### Android

1. Download `RaceBoxCrewChief-1.4.0-hackathon.apk` from Releases.
2. Allow installation from the browser/file manager when Android asks.
3. Open the app and tap **Try offline demo**.

This is a clearly labelled sideloadable hackathon APK, not a Play Store release.
The demo conversation is generated locally and does not contact a gateway.

## Use your own OpenClaw connection

Live Crew Chief features require:

- an OpenClaw installation you already control;
- Tailscale on the gateway host and client devices, or an HTTPS reverse proxy
  you operate;
- Python 3.11 or newer on the gateway host.

The release includes two thin setup helpers in `gateway/`:

- `setup-gateway.ps1` configures a **Windows gateway host** and can optionally
  add an at-logon Task Scheduler entry;
- `setup-gateway.sh` configures a **Linux gateway host** and can optionally add
  a systemd user service.

The Linux script is not a Viewer installer. The Viewer remains a Windows app;
Linux is simply one supported place to run the small gateway beside OpenClaw.

Windows gateway example:

```powershell
cd gateway
.\setup-gateway.ps1 -TailscaleIp <your-tailscale-ip> -InstallStartup
```

Linux gateway example:

```bash
cd gateway
bash ./setup-gateway.sh --tailscale-ip <your-tailscale-ip> --install-startup
```

Omit the startup option to run it manually. Add `--dry-run` (PowerShell:
`-DryRun`) to inspect actions first. Each successful setup creates a rollback
manifest. See [Public connection guide](docs/public-connection-guide.md).

The helper does **not** install OpenClaw, copy provider credentials, or expose
the OpenClaw operator token. It calls `openclaw agents add`, enables OpenClaw's
loopback Chat Completions endpoint, installs the RaceBox skill/instructions,
applies a minimal no-tools policy, and generates an independent RaceBox client
token.

OpenClaw references: [HTTP API](https://docs.openclaw.ai/gateway/openai-http-api)
and [agent CLI](https://docs.openclaw.ai/cli/agents).

## Connect the apps

In Windows, open **Crew Chief > Connection**, choose **My OpenClaw gateway**,
enter the URL reported by your helper, and optionally paste the generated
RaceBox client token. The URL is stored in a local JSON settings file; the token
is stored in Windows Credential Manager and is never written into that JSON.

On Android, choose **Connect to my gateway**. Scan the Viewer QR code or enter
the same gateway base URL and six-digit pairing code manually. The app asks for
confirmation before using an unfamiliar host. The session gateway, session
token, messages, and drafts are encrypted with a non-exportable Android
Keystore key.

HTTP is accepted by the apps only for Tailscale's `100.64.0.0/10` address
range (and Windows loopback for a local gateway). Any other host must use HTTPS.

## Privacy and security boundary

```text
Windows Viewer / Android app
        |  RaceBox client or session token
        v
Narrow RaceBox gateway
        |  OpenClaw operator credential stays here
        v
OpenClaw loopback HTTP endpoint -> user's selected model/provider
```

- No personal IP address, active endpoint, provider credential, operator token,
  or default live connection is included in source or release artifacts.
- Offline demo mode makes no network calls.
- Raw telemetry files and original filesystem paths are not placed in model
  prompts. Deterministic evidence and bounded context are sent for live analysis.
- The latest Crew Chief system prompt is attached to every inference call. Its
  revision appears in authenticated `/health` output.
- `racebox-crew-chief-v14`, `racebox-companion-session-v1`, existing routes,
  payloads, and QR pairing format remain compatible.

## Model selection

The setup helper creates the `racebox-crew-chief` OpenClaw agent. It uses the
OpenClaw default model unless you pass `--model <provider/model>` (PowerShell:
`-Model <provider/model>`). Model choice does not raise telemetry confidence:
the prompt always separates measured association from proof of causation.

## Troubleshooting

- **Offline demo works, live connection fails:** run the helper's dry run, check
  `tailscale status`, and confirm the authenticated `/health` response.
- **Android rejects HTTP:** the address must be in `100.64.0.0/10`; otherwise
  use HTTPS.
- **Pairing code expired:** create a new code in the Viewer. Codes are short-lived
  and one-use.
- **OpenClaw returns 404 for chat completions:** rerun setup; that endpoint is
  disabled by default and the helper enables it locally.
- **Wrong prompt/contract:** `/health` must report the versions at the top of
  this README.

## Known limitations

- The Windows hackathon ZIP and Android APK are unsigned.
- Android is sideload-only.
- Docker is not included.
- Live phone pairing is unavailable while the Windows Viewer is in offline demo
  mode; the phone has an independent offline demo.
- A user-owned OpenClaw/Tailscale or HTTPS deployment is required for live AI.

## Source layout and development

- Repository root: Windows Viewer and portable C++ core
- `android/`: Android companion
- `gateway/`: Python gateway and Windows/Linux setup helpers

Windows build:

```powershell
cmake --preset windows-distribution
cmake --build --preset distribution --parallel
ctest --preset distribution
pwsh -File scripts/package.ps1 -Configuration Release
```

Android build:

```powershell
cd android
.\gradlew.bat testDebugUnitTest assembleDebug
```

Gateway tests:

```powershell
cd gateway
python -m unittest -v test_gateway.py test_setup_common.py
```

License: [MIT](LICENSE). Dependency notices: [Third-party notices](THIRD-PARTY-NOTICES.md).
