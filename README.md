# ScarletNX
<p align="center">
  <img src="./dist/wimsy.svg" alt="Wimsy, the mascot" width="200"><br>
  <b>Wimsy, the ScarletNX mascot</b>
</p>

Discord: https://discord.gg/tRTRXXbyjp (my god why don't we just use IRC like good people?)

A GPLv3 static ARM64 to x86 recompiler with a Nintendo Switch translation layer for Linux, Windows and FreeBSD. Forked from MZLab's SwitchRecomp.

To build simply:
```bash
git clone --depth=1 https://github.com/scarletnx/ScarletNX
cd ScarletNX
cmake --preset default -B build && cmake --build build
```

Afterwards, put `prod.keys` (and `title.keys`, for NSPs) in `~/.switch`, or pass them with `--keys` and
`--title-keys`.
```bash
./build/scarletnx "Game.xci"  # extract, recompile, then play (also accepts NSP)
```
Run the same command again after an interruption. Completed extraction and recompilation are skipped,
but the game always starts anew. To run the steps separately:
```bash
./build/scarletnx --extract "Game.xci" --name Game  # -> work/Game/{exefs,romfs}
./build/scarletnx --recompile work/Game Game        # -> games/Game
cd games && ./Game
```

For more detailed information please see: [Build + run](./docs/BUILD.md).

# Special thanks

- yuzu, Suyu, Torzu, Ryujinx, [Ryubing](https://git.ryujinx.app/ryubing/ryujinx) (Ryujinx's continuation), [Citron](https://git.citron-emu.org/citron/emulator) and [Eden](https://git.eden-emu.dev/eden-emu/eden).
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
- LibHac.
- [Nexium](https://nexium-emu.org/) for research and development.
- [Switchbrew](https://switchbrew.org) for research.

Wimsy, the mascot and the runtime's icon (`dist/wimsy.svg`) was drawn by Liz.
