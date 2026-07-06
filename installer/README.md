# Mettle Installer

Native Windows setup wizard built with [Inno Setup 6](https://jrsoftware.org/isinfo.php). Produces a single `Mettle-Setup.exe` — no PowerShell required to run it.

## Features
- Copies `mettle.exe`, `stdlib\`, and `runtime\` to the installation directory.
- Installs opt-in helper objects (`crash_handler.o`, `atomics.o`, etc.) so `--build` can link them on demand.
- Installs `mettle-build.bat` as a thin wrapper over `mettle --build`.
- Per-user or all-users install (chosen at launch; UAC only when needed).
- Optional PATH and `.mettle` file association tasks.
- Built-in uninstaller registered in Windows Settings.
- Modern Inno 6 wizard UI (HiDPI, Windows light/dark aware) — no custom bitmap assets.

## Build the installer

1. Build the compiler first: `build.bat gcc` from the repo root.
2. Install [Inno Setup 6](https://jrsoftware.org/isinfo.php).
3. Compile:
   ```bat
   "C:\Path\To\Inno Setup 6\ISCC.exe" installer\Mettle.iss
   ```
   Or stamp a version explicitly:
   ```bat
   ISCC.exe /DMyAppVersion=0.13.0 installer\Mettle.iss
   ```
4. Output: `installer\out\Mettle-Setup.exe`

CI builds the same way in `.github/workflows/release.yml`.

## After installation

```bat
mettle --version
mettle --build my_program.mettle
mettle help
```

`mettle-build.bat my_program.mettle` forwards to `mettle --build`.

## Other install paths

- **GUI (this installer):** `Mettle-Setup.exe` — full wizard, uninstaller, file associations.
- **Script (download latest release):** `install.ps1` at the repo root — one-liner for `%LOCALAPPDATA%\Mettle`, no admin.
