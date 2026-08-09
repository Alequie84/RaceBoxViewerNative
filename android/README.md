# RaceBox Crew Chief for Android

Version `1.4.0` is a sideloadable hackathon companion to the Windows Viewer.

The first screen offers:

- **Try offline demo** — scripted Richmond answers with no network calls.
- **Connect to my gateway** — scan a Viewer QR code or enter a user-owned
  gateway URL and six-digit code.

The app confirms unfamiliar hosts. HTTPS is required except for Tailscale IPv4
addresses in `100.64.0.0/10`. Each paired gateway URL, session token, message
cache, and draft is encrypted with Android Keystore.

Build and test:

```powershell
.\gradlew.bat testDebugUnitTest assembleDebug
```

The APK is written under `app/build/outputs/apk/debug/`.
