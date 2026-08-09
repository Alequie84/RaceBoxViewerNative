# Public release handoff

This branch is the public, self-contained RaceBox hackathon release.

- Windows version: `2.0.0.020`
- Android version: `1.4.0` / code `5`
- Gateway contract: `racebox-crew-chief-v14`
- Prompt revision: `racebox-crew-chief-behavior-v14-public-1`
- Default connection mode: offline demo with no network
- Live connection: user-supplied gateway only

Never add a personal IP, provider credential, OpenClaw operator token, client
token, private key, or local absolute path. Test setup helpers with `--dry-run`
or an isolated `--install-root`. Release artifacts must pass the repository
secret and personal-endpoint scan before publication.
