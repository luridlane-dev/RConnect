# RConnect for Android

Add servers with an IP address, an administrator user name and a password, and see who is connected
now and who signed in or out. It talks to each server over WinRM (port 5985, NTLM with message
encryption), like the desktop program, so the phone has to reach the servers (same network or VPN).

Made for long lists:

- **Servers**: the four numbers on top (all, in use, free, problem) are filters; search finds a
  server, group, person or computer; servers are grouped (for example by site) and groups collapse.
- **People**: everyone signed in anywhere, in one list.
- **Server page**: who is connected now, sign-in/out history (2 days) and failed sign-in attempts.
- **Import list** (⋮ menu): paste one server per line, `ip;user;password;name;group` (name and group
  optional), or rows copied from a spreadsheet.
- Checks run 8 at a time, every minute while the app is open; pull down to check everything now.

Passwords are encrypted with a key in the phone's keystore and the app does not allow backups.
Release builds block screenshots and task-switcher previews.

- `app/src/main/java/com/rconnect/android/winrm`: the WinRM client (`Ntlm`, `WinRmClient`) and the
  parsers (`Parsing`). The NTLM code is tested against the examples of MS-NLMP.
- Build: `gradlew assembleDebug`; tests: `gradlew testDebugUnitTest`. Needs the Android SDK
  (`local.properties` with `sdk.dir`) and JDK 17+.
- `LiveServerTest` reads a real server and is skipped unless `RC_HOST`, `RC_USER` and `RC_PASS` are set.
