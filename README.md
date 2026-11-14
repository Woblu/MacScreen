# Toolscreen for macOS

A macOS port of [Toolscreen](https://github.com/jojoe77777/Toolscreen), the screen mirroring and overlay tool for Minecraft Java Edition speedrunning. Resize your game, add overlays, and use EyeZoom — all without OBS projectors or leaving fullscreen.

The settings, config files and in-game interface are the same as Toolscreen on Windows. This is an unofficial port; it is not made or supported by the Toolscreen developers.

> **Not approved by [speedrun.com](https://www.speedrun.com/mc) or [MCSR Ranked](https://mcsrranked.com/).** Don't use it for runs you submit or in ranked matches unless it has been approved.

<p align="center">
  <img src="docs/images/screenshot.png" alt="Toolscreen screenshot" width="800">
</p>

## Features

- **Screen Mirroring** — Mirror parts of your screen to other monitors or positions with configurable capture zones
- **EyeZoom** — Magnify the area around your crosshair for precise ender eye readings
- **Mode System** — Create presets (Fullscreen, Thin, Wide, EyeZoom, custom) and switch between them with hotkeys
- **Image Overlays** — Display images on top of your game (e.g. Ninjabrain Bot)
- **Window Overlays** — Capture and display any window directly in-game (e.g. a timer, Ninjabrain Bot)
- **Key Rebinding** — Remap keys and configure custom input behavior
- **Hotkey System** — Bind mode switching, overlay toggling, and sensitivity adjustments to any key

### Differences from Windows

- Minecraft 26.2 and newer are not supported.
- The Windows mouse speed setting has no effect.
- Window overlays need the Screen Recording permission (macOS asks the first time).
- The virtual camera sends to OBS's virtual camera. Start OBS's virtual camera once first so macOS installs it.

## Requirements

- **macOS 13.3 or newer** (Apple Silicon or Intel)
- **Java 17+** (needed to run the installer)
- **Minecraft Java Edition** 1.13 to 26.1
- **Supported launchers:** Prism Launcher, MultiMC

## Installing

1. Download the `.jar` from the latest release
2. Locate your instance folder:
   - **Prism Launcher**: right-click instance → "Folder"
   - **MultiMC**: right-click instance → "Instance Folder"
3. Copy the `.jar` into the instance folder and double-click it (or run `java -jar` on it in Terminal)
4. Follow the on-screen instructions. If the launcher is running, the installer asks to restart it.
5. Launch Minecraft and look for the Toolscreen popup in the top-left corner

The installer copies Toolscreen to `~/.config/toolscreen/dlls` and adds `-agentpath:` with that path to the instance's JVM arguments. It saves the instance's previous Java settings so **Uninstall** can put them back.

## Configuration

Press <kbd>Ctrl</kbd> + <kbd>I</kbd> in fullscreen to open settings.

- **Basic Mode** — The most common settings, simplified for quick setup
- **Advanced Mode** — Full access to all features and fine-grained control

Hover over the `(?)` icons for tooltips explaining each setting.

## Uninstalling

Double-click the installer `.jar` in the instance folder and select **Uninstall**. The instance's JVM arguments are restored to what they were before installing.

## Reporting problems

Report problems with the macOS port in this repository's issues, not in the Toolscreen Discord.

## Building

Requires Xcode Command Line Tools, CMake and a JDK 17+.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build
cmake --build build --target macos_package
```

The installer is written to `build/package/`. All macOS code is in `src/platform/macos`; the rest of the source is upstream Toolscreen.

## Credits

Toolscreen is made by [jojoe77777](https://github.com/jojoe77777). The macOS port is by [Woblu](https://github.com/Woblu), written with Claude.

MIT License — see [LICENSE](LICENSE).
