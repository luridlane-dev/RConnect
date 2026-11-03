# RConnect for Android

Shows who is connected to your Remote Desktop servers and who signed in or out. It talks to each
server over WinRM (port 5985, NTLM with message encryption), like the desktop program, so the phone
has to reach the servers (same network or VPN).

- **Servers**: grouped list with search and status filters; the server page shows who is connected,
  sign-in and sign-out history (2 days) and failed sign-ins.
- **People**: everyone signed in anywhere, in one list.
- **Contacts**: nicknames for client computers, shared with the desktop program (`rconnect-contacts`).
- **Import and export**: the desktop program's server file (optionally encrypted with a passphrase),
  or one server per line: `ip;user;password;name;group`.
- **Notifications**: someone connects or leaves, or a server stops answering. When the app is closed
  the phone checks about every 15 minutes; Android 13 and later ask for permission.

Passwords are encrypted with a key in the phone's keystore and backups are disabled. Release builds
block screenshots.

Build: `gradlew assembleDebug`. Needs the Android SDK (`local.properties` with `sdk.dir`) and JDK 17+.
