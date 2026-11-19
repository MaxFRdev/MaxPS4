# MaxPS4 — experimental PS4 emulator for iPhone

[![Platform](https://img.shields.io/badge/Platform-iOS%20ARM64-222222)](https://github.com/MaxFRdev/MaxPS4)
[![Status](https://img.shields.io/badge/Status-experimental-orange)](https://github.com/MaxFRdev/MaxPS4/actions)
[![License](https://img.shields.io/badge/License-GPL--2.0-blue)](LICENSE)
[![Playable games](https://img.shields.io/badge/Playable%20games-Sonic%20Mania%20%C2%B7%20Mortal%20Kombat-brightgreen)](#what-works-today)

<p align="center">
  <a href="https://discord.gg/99KcfSVxT8">
    <img src="https://img.shields.io/badge/Discord-Join%20the%20MaxPS4%20community-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Join the MaxPS4 Discord">
  </a>
</p>

<p align="center"><b>💬 Follow progress, ask questions and share your tests on the <a href="https://discord.gg/99KcfSVxT8">MaxPS4 Discord</a>.</b></p>

**MaxPS4** is an independent PS4 emulation project for iPhone. Games' x86-64 code runs on **FEXCore** and its ARM64 JIT, whose activation on iPhone through StikDebug is still experimental; the PS4 system functions are rewritten in MaxPS4, and the PS4 GPU is translated to **Metal**. The project builds on work from **AetherPS4**, **FEXCore** and **shadPS4**.

## 🚧 Development progress — November 2026

> **Experimental version:** a successful build or a game that boots does not prove its gameplay works. The features below are being validated on real devices.

| Work item | Progress | Tracking |
| --- | --- | --- |
| FEXCore JIT on iOS with LiveContainer and StikDebug | **Merged into `main`**; activation confirmed on iPhone in a user test; some games still fail to launch | [PR #325](https://github.com/MaxFRdev/MaxPS4/pull/325) |
| Game and PRX module compatibility | Experimental work in progress; check each game on device | [Pull requests](https://github.com/MaxFRdev/MaxPS4/pulls) |
| Mortal Kombat Trilogy (CUSA53178) | **Playable on iPhone**: intro and menu (~56–60 FPS), PS1 game launched from the menu, fights and sound confirmed in a user test, about 55 FPS in fights | [PR #329](https://github.com/MaxFRdev/MaxPS4/pull/329) |
| GNM/PM4 GPU to Metal | Fixes and compatibility tests in progress | [Pull requests](https://github.com/MaxFRdev/MaxPS4/pulls) |

### LiveContainer and SideStore

- **SideStore**: installation method described below; enabling the JIT still requires the proper StikDebug permissions and setup.
- **LiveContainer**: in a test on iPhone 15 Pro Max (iOS 27.2), StikDebug opened and MaxPS4 reported the JIT as active with the build from [PR #325](https://github.com/MaxFRdev/MaxPS4/pull/325), now merged into `main`. This is a **one-off validation**, not a compatibility guarantee for every device. See [Enabling the JIT with LiveContainer](#enabling-the-jit-with-livecontainer).
- **Mortal Kombat Trilogy (CUSA53178)**: the startup memory crashes are fixed by [PR #329](https://github.com/MaxFRdev/MaxPS4/pull/329); the game's built-in PS1 emulator runs with fights and sound. The `HW render: callback setup error` message the game prints is expected: its launcher always refuses libretro hardware rendering, on a PS4 too.

[PR #325](https://github.com/MaxFRdev/MaxPS4/pull/325) is merged into `main` and supersedes PRs #319, #320, #322, #323 and #324; the SideStore IPA built from `main` includes it. [PR #329](https://github.com/MaxFRdev/MaxPS4/pull/329) is merged and supersedes PR #326. An active JIT does not mean a game is playable.

## What works today

**Three games run on iPhone 15 Pro Max**, tested with the author's own game folders:

- **Sonic Mania**: close to 60 frames per second, sound, controller and saves.
- **Mortal Kombat: Legacy Kollection**: 60 frames per second in fights, about 54 when the phone heats up, with sound.
- **Mortal Kombat Trilogy**: menu at 56–60 frames per second, PS1 game at about 55 in fights, with sound and controls.

Other games are not supported or not tested yet (see [Compatibility](#compatibility)).

### Emulation

| Part | What is done |
| --- | --- |
| CPU | Game x86-64 code run by the FEXCore JIT on iPhone (StikDebug), several game threads in parallel, runtime built in optimized mode |
| Loading | Complete game folder: `eboot.bin` (plaintext SELF) and `sce_module/*.prx` modules, linked together |
| PS4 system | Files, memory, threads, mutexes, condition variables, event queues, **semaphores**, **event flags**, precise clocks and time counters |
| GPU | GNM/PM4 commands decoded and GCN shaders translated to Metal; native textures filtered by the iPhone GPU; depth test and face culling (tested on Mac, 3D rendering still to be validated on iPhone); 8-, 16- and 32-bit color targets (including 16-bit float). No stencil yet: most 3D games do not display correctly yet |
| Display | Up to **60 frames/s**, synced to the iPhone's screen refresh; Fit / Fill / Stretch; 1080p or 720p reported to the game |
| Sound | Game audio output (AudioOut) played on the iPhone |
| Controller | MFi, Xbox, DualShock 4 and DualSense controllers: all buttons, analog sticks and triggers, touchpad; adjustable touch controls (size, opacity, L1/L2/R1/R2), hidden as soon as a controller is connected |
| Saves | Real PS4 saves (`sceSaveData`) stored in **On My iPhone › MaxPS4 › Sauvegardes › <game ID>** |
| Power | A game waiting for the screen sleeps instead of spinning |

### App

- **Library:** games copied into **On My iPhone › MaxPS4 › Jeux** with the Files app show up automatically, with their title and icon; a folder picked with **Play** also stays in the library.
- **Clean home screen:** game list with a Play button, **Resume** the last game launched.
- **Game page:** artwork, ID, version, space used by saves and direct access to their folder.
- **Pause:** opening the game menu pauses the game.
- **Switch games:** quit a game and launch another without a long wait.
- **Per-game settings:** display, resolution and controls remembered for each game.
- **FPS counter:** optional on-screen frames-per-second counter, from the in-game menu.
- **6 languages:** French, English, Spanish, German, Italian, Portuguese (Brazil), following the iPhone's language or chosen in Settings.
- **Compatibility report:** when a game stops, the app says why (missing PS4 functions, error) and saves a report in **MaxPS4 › Rapports**, ready to share.
- **Developer mode** (Settings): test options and technical reports, hidden by default.

## Compatibility

| Game | Status |
| --- | --- |
| Sonic Mania | ✅ Playable (tested by the author on iPhone) |
| Mortal Kombat: Legacy Kollection | ✅ Playable (tested by the author on iPhone) |
| Mortal Kombat Trilogy | ✅ Playable (tested by the author on iPhone, about 55 FPS in fights) |
| Call of Duty: Modern Warfare Remastered | 🔧 In progress: loading fixes under test ([PR #333](https://github.com/MaxFRdev/MaxPS4/pull/333)) |
| Crash Bandicoot 4 | ❌ Blocked: iOS refuses the fixed memory addresses Unreal Engine 4 games need |
| Other light 2D games and homebrews | ❓ Not tested: worth trying, with the compatibility report |
| Large 3D games (God of War, Assassin's Creed…) | ❌ Out of reach for now (CPU, GPU and memory) |

A game that gets stuck leaves a report in **MaxPS4 › Rapports**: share it on [Discord](https://discord.gg/99KcfSVxT8) or in an [Issue](https://github.com/MaxFRdev/MaxPS4/issues) to help add what is missing.

## Install and play

1. Download the IPA built by the [MaxPS4 FEXCore SideStore IPA](https://github.com/MaxFRdev/MaxPS4/actions/workflows/fexcore-app.yml) workflow (artifact **MaxPS4-FEXCore-SideStore.ipa**; GitHub may provide a ZIP archive: extract it to get the `.ipa` file before importing it into LiveContainer or SideStore) and install it with **SideStore**.
2. In StikDebug, import the **same PairingFile** SideStore uses, then set up the connection and VPN following the StikDebug guide. The PairingFile allows pairing; it does not enable the JIT by itself.
3. Go back to MaxPS4 and tap **JIT** to start the StikDebug activation. See [the SideStore + StikDebug protocol](docs/fexcore-stikdebug-jit.md), or [the LiveContainer steps](#enabling-the-jit-with-livecontainer).
4. Copy your complete game folder (`eboot.bin`, `sce_module`, `sce_sys` and its data) into **On My iPhone › MaxPS4 › Jeux**, then tap **Play**.

### Enabling the JIT with LiveContainer

MaxPS4 needs the JIT to run games. Under LiveContainer, MaxPS4 itself asks StikDebug to enable it: it sends StikDebug its own process and a built-in activation script, so there is nothing to copy by hand.

**Requirements**
- **SideStore** installed and working, with **LiveContainer** installed through SideStore.
- **StikDebug** installed, with the **same PairingFile** as SideStore, and its connection/VPN set up following the StikDebug guide.
- The **MaxPS4-FEXCore-SideStore.ipa** IPA (extract the `.ipa` if GitHub provides a ZIP).

**Steps**
1. In LiveContainer, import the MaxPS4 IPA.
2. Open MaxPS4's settings in LiveContainer (long-press the app) and enable **Use LiveContainer's Bundle ID**. Without it, MaxPS4 refuses the activation: StikDebug would not find the right process.
3. If LiveContainer offers a **JIT enabler**, choose **StikDebug**.
4. Launch MaxPS4 from LiveContainer, tap **JIT**, then **Enable with StikDebug**.
5. StikDebug opens and runs MaxPS4's script. Wait for the script to finish, then go back to MaxPS4: the screen should show **JIT ready**.
6. Launch a game. The JIT must be enabled again each time MaxPS4 is relaunched.

**If it fails**, MaxPS4 shows the cause (these messages are currently in French only):
- *Identifiant du processus absent… activer Use LiveContainer's Bundle ID* (process ID missing): redo step 2, then relaunch MaxPS4.
- *iOS n'a pas ouvert StikDebug* (iOS did not open StikDebug): check that StikDebug is installed and, in LiveContainer, the JIT enabler and Use LiveContainer's Bundle ID.
- StikDebug opens but the JIT stays inactive: check StikDebug's PairingFile and VPN, then try again.

The PairingFile is specific to your iPhone: keep it private and never attach it to MaxPS4 reports. This method was validated on iPhone 15 Pro Max (iOS 27.2); it is not a guarantee for every device.

MaxPS4 decrypts nothing: you need an already-extracted game folder from games you own. Development continues in several experimental branches and pull requests; check the progress table above for recent work.

## Compliance and responsibility

- **Licenses**: MaxPS4 states the [GNU GPL v2.0](LICENSE). Every third-party source or dependency must be attributed, audited and redistributed under its own license. No complete compliance audit is claimed.
- **Attribution**: contributions and intellectual property of the original authors are preserved. [shadPS4](https://github.com/shadps4-emu/shadPS4), [FEX](https://github.com/FEX-Emu/FEX) and [AetherPS4](https://github.com/Leviidev/AetherPS4) are separate projects.
- **Trademarks**: MaxPS4 is independent and **is not affiliated with Sony Interactive Entertainment / PlayStation or with the official teams of the projects cited**.
- **Content**: no commercial game, PS4 firmware, key, protected encrypted content or PKG file is provided, and MaxPS4 circumvents no protection. Only use games you own.
- **App Store**: publishing on the App Store is not a goal; experimental IPA distribution remains subject to the licenses and to iOS signing restrictions.

See the detailed document [Compliance, attribution and legal limits](docs/LEGAL-COMPLIANCE.md) (in French).

## Contributing and community

Contributions are welcome through [Issues](https://github.com/MaxFRdev/MaxPS4/issues), [Pull Requests](https://github.com/MaxFRdev/MaxPS4/pulls) and [CONTRIBUTING.md](CONTRIBUTING.md). [GitHub Actions](https://github.com/MaxFRdev/MaxPS4/actions) build the IPA and run the tests; development builds are not stable releases.

[![Discord](https://img.shields.io/badge/Discord-Join%20MaxPS4-5865F2?logo=discord&logoColor=white)](https://discord.gg/99KcfSVxT8)

**[Join the MaxPS4 Discord](https://discord.gg/99KcfSVxT8)**

## Technical documentation

- [PS4 GPU translation to Metal (Sonic Mania)](docs/sonic-gpu.md)
- [VideoOut and display](docs/videoout-linear.md)
- [PS4 system services](docs/ps4-system-services.md)
- [Orbis runtime foundation](docs/orbis-runtime-foundation.md)
- [Orbis/SCE ELF and PRX analysis](docs/orbis-sce-prx-analysis.md)
- [FEXCore IPA for SideStore](docs/fexcore-app-sidestore.md)

---
*Last updated: November 19, 2026 (tracking experimental work).*
