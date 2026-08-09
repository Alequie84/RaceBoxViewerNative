# Contributor instructions

Keep this repository safe for public distribution.

- Do not commit credentials, private keys, personal endpoints, or machine-specific paths.
- The apps must default to offline demo and require an explicit user-supplied live connection.
- Keep the OpenClaw operator credential on the gateway host only.
- Preserve `racebox-crew-chief-v14`, `racebox-companion-session-v1`, and the pairing URI format.
- Update the prompt revision and authenticated `/health` together when Crew Chief instructions change.
- Run Windows, Android, gateway, packaging, and artifact scans before a release.
