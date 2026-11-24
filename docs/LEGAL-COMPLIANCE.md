# Compliance, licenses and attribution — MaxPS4

**Status: porting work in progress, full compliance not yet audited.**

## Origin and independence

MaxPS4 is an experimental, independent project on iOS. It is **not developed, endorsed or affiliated** with Sony Interactive Entertainment, PlayStation or the official shadPS4 team. The PlayStation, PS4 and Sony trademarks belong to their respective owners.

shadPS4: https://github.com/shadps4-emu/shadPS4 — thanks to its authors and contributors.

AetherPS4: https://github.com/Leviidev/AetherPS4 — its authors keep the rights and notices applicable to their contributions.

The MaxPS4 repository contains a Swift interface, ELF/CPU components and native bridges. CI fetches AetherPS4 and FEX sources to build real components (including the guest engine with the Darwin FEXCore headers). Fetching the complete shadPS4 sources was proposed in PR #83; that alone is not a port of the kernel or the GPU. A few games are playable on iPhone (see the [README](../README.md#compatibility)), and the iPhone JIT has only been validated in individual user tests. None of these projects should be presented as a complete port.

## License and redistribution

MaxPS4 includes the text of the GNU GPL version 2 in its [LICENSE](../LICENSE) file. shadPS4 is distributed under GPL-2.0. Before incorporating its source code and redistributing a derived application, you must:
- keep the copyright notices, licenses and notices required by the authors;
- identify modified files and document their changes;
- provide access to the corresponding source code and meet the GPL distribution obligations that apply to binaries;
- review **all dependencies and resources** (including their own licenses), and check that the whole is compatible, in particular with iOS signing/distribution terms;
- not reuse logos, trademarks or other people's works without permission when they are not covered by a suitable license.

This document **is not a legal certification**. An audit of the code actually integrated and of each distribution method is needed before presenting MaxPS4 as fully compliant.

## Games, firmware and data

MaxPS4 provides no commercial game, PKG, key, PS4 firmware file or Sony protected content. Users must hold the necessary rights to their own files. Owning a game does not automatically settle every legal question about decryption, technical protection measures or extraction under applicable law. Do not distribute circumvention tools or promise universal legality.

## Development distribution outside the App Store

The project currently favors IPA testing outside the App Store. This does **not** exempt distributions from license, attribution, corresponding-source, signing obligations or iOS security restrictions. An Apple Developer membership does not automatically grant the right or the ability to run a JIT on iOS devices. Compliance of the complete product is not certified.

## Technical limits and transparency

A successful iOS build does not mean emulation works. PKG scan screens and ELF/CPU tests do not prove a PS4 game can start. x86-64 execution on ARM64, PS4 services and the graphics engine are still being developed and tested.

References:
- https://github.com/shadps4-emu/shadPS4
- https://github.com/Leviidev/AetherPS4
- https://github.com/shadps4-emu/shadPS4/blob/main/LICENSE
- https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
