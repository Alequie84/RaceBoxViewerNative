# RaceBox Hackathon Public Release 2.0.0.020

This release combines the latest Windows Viewer, Android Crew Chief companion,
and a cross-platform user-owned OpenClaw gateway.

Highlights:

- Safe, clearly labelled Richmond offline demos on Windows and Android.
- No bundled personal connection or credential; live users supply their own gateway.
- Windows URL settings plus token storage in Windows Credential Manager.
- Android QR/manual gateway pairing, unfamiliar-host confirmation, strict
  HTTPS/Tailscale URL policy, and Android Keystore-encrypted session connection data.
- Windows and Linux gateway setup helpers with optional startup registration,
  rollback, independent RaceBox client tokens, minimal OpenClaw agent tools, and
  authenticated prompt-revision health reporting.
- MIT license, dependency notices, public setup/privacy documentation, and CI
  for Windows, Android, gateway, macOS preview, and public-release scanning.

Compatibility remains `racebox-crew-chief-v14` and
`racebox-companion-session-v1`. Live OpenClaw features require the end user's
own OpenClaw and Tailscale or HTTPS connection. Alex's private deployment is not
included and is unchanged.
