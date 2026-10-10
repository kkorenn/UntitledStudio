# Releasing Untitled Studio

## Version

The only place the version lives is `project(UntitledStudio VERSION x.y.z)` in `CMakeLists.txt`.
It flows into Info.plist, the About window, the splash screen, crash reports, the update check
and the DMG name. Bump it there, then update `update.json` (see below).

## macOS

```
scripts/package-macos.sh
```

builds `build-release/` (Release), copies the app into a staging folder with an `Applications`
link and `LICENSE.txt`, signs it, and writes `dist/UntitledStudio-<version>-macOS.dmg` (about 13 MB).

The installer window is styled: `resources/dmg/background.svg` (version stamped in, rendered 1x + 2x
with `rsvg-convert`, `brew install librsvg`) and `resources/dmg/dmg-settings.py` (window size, icon
positions, disk icon). `dmgbuild` (MIT, build tool only, not shipped) is installed automatically into
`build-release/dmg-venv` and writes Finder's layout directly, so no Finder scripting is needed.

### Without an Apple Developer account (default)

The app is **ad-hoc signed** (`codesign -s -`). It runs on the machine that built it; on other Macs
Gatekeeper blocks it the first time, and users have to right-click the app > Open (or allow it in
System Settings > Privacy & Security). Fine for testers, not for a public release.

### Signed + notarized (needs the Apple Developer Program, $99/year)

1. Join the Apple Developer Program, then create a **Developer ID Application** certificate
   (Xcode > Settings > Accounts > Manage Certificates) so it's in your login keychain.
2. Create an app-specific password at https://account.apple.com (Sign-In and Security).
3. Run:

   ```
   DEVELOPER_ID_APP="Developer ID Application: Your Name (TEAMID1234)" \
   APPLE_ID=you@example.com TEAM_ID=TEAMID1234 APP_PASSWORD=xxxx-xxxx-xxxx-xxxx \
   scripts/package-macos.sh
   ```

The script then signs with the hardened runtime (`--options runtime --timestamp`) and
`resources/entitlements.plist`, signs the DMG, submits it with `xcrun notarytool submit --wait`
and staples the ticket (`xcrun stapler staple`). Without `APPLE_ID`/`TEAM_ID`/`APP_PASSWORD` it
signs but skips notarization.

Entitlements, and why:

| Entitlement | Why |
|-------------|-----|
| `com.apple.security.device.audio-input` | Recording (the hardened runtime blocks the microphone otherwise) |
| `com.apple.security.cs.disable-library-validation` | Loading third-party AU / VST3 plugins signed by other teams |
| `com.apple.security.cs.allow-unsigned-executable-memory` | Some plugins (copy protection, JIT DSP) need it or they crash on load |

Not sandboxed (plugin hosting and free file access don't work in the App Sandbox).

## Windows (planned, not built yet)

- Installer: Inno Setup (simplest) or WiX (MSI, for managed installs). Install to
  `Program Files\Untitled Studio`, Start menu shortcut, `.usp` file association, uninstaller.
- Code signing: an OV or EV code-signing certificate (EV avoids SmartScreen warnings faster),
  `signtool sign /fd sha256 /tr <timestamp-url> /td sha256` on the .exe and the installer.
- The app links the MSVC runtime statically, so no VC++ redistributable is needed.

## Updates

The app can check a JSON feed (see BUILDING.md, "Update check"); it's off unless the build sets
`UNTITLEDSTUDIO_UPDATE_URL`. The simplest host is the repo itself: build with
`-DUNTITLEDSTUDIO_UPDATE_URL=https://raw.githubusercontent.com/kkorenn/UntitledStudio/master/update.json`
and, for each release, edit `update.json`:

```json
{ "version": "0.2.0", "url": "https://github.com/kkorenn/UntitledStudio/releases/latest", "notes": "What changed" }
```

Only `http(s)` download links are accepted. The app only shows "Version X is available" with a
button that opens the download page; it doesn't download or install anything itself.

## Crash reports

Local only: `~/Library/Untitled Studio/CrashReports/` (version, OS, backtrace, recent log lines).
The next launch offers View / Copy / Reveal / Dismiss, combined with the autosave recovery prompt.
Nothing is uploaded; `Release::Crash` in `Source/Release.h` marks where an upload would go
(ask the user first).

## Licensing (GPLv3) checklist for every release

Untitled Studio uses JUCE (AGPLv3) and Tracktion Engine (GPLv3) under their free licenses, so it
is distributed under the GPLv3 (`LICENSE`); see `THIRD_PARTY_LICENSES.md`.

- **Offer the source.** Everyone who gets a binary must be able to get the exact source it was
  built from, including `libs/` at the same commits. Easiest: tag the release
  (`git tag v0.1.0`), push the tag to https://github.com/kkorenn/UntitledStudio and link the tag
  from the release page. The About window links the repo.
- Ship `LICENSE` (the DMG has `LICENSE.txt`; the app has `Contents/Resources/licenses/`).
- Keep `THIRD_PARTY_LICENSES.md` current when adding a library (and add its license file to the
  list in `CMakeLists.txt`).
- Don't bundle LAME (MP3 export runs a user-installed `lame`) or Steinberg's ASIO SDK.
- Going closed-source later would need commercial JUCE and Tracktion Engine licenses.
