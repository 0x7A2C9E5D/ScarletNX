<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Status and planned work

Recorded game-validation status from 2026-10-11: Catherine: Full Body USA
(`0100BF00112C0000`, v2.0) reached the New Game cutscene at 60 FPS using Vulkan, without sound.
Earlier crash addresses refer to the Asian release (`0100BC4010896000`).
These game results were not rerun for the 2026-10-12 [audit](AUDIT.md).

The Clang/CMake build, cross-platform CI, SDL3 backend, Vulkan renderer, shader caches and
component layout are in place. Git history records completed fixes and measurements.

## Next work

- Address the high-priority correctness findings in [AUDIT.md](AUDIT.md).
- Add instruction, IPC, memory and GPU-cache regression tests. Extend smoke runs with
  reference-image checks and display-free execution.
- Enable broader compiler warnings. Check vector operands near register 255 before indexing arrays.
- Implement audio output, PCM playback for `audout`, and mixing/routing/effects for `audren`.
  Validate hwopus with audible output.
- Schedule GPU channels separately so a blocked channel cannot stall work that satisfies its wait.
- Keep vsync progressing during Windows window dragging. Test native Windows and real gamepads.
- Support framebuffer transforms, attachment feedback and BCn fallback where required.
- Match ARM NaN selection/default-NaN behavior and directed rounding in generated FP operations.
- Honor thread suspension at WFE boundaries.
- Measure Vulkan performance, profile runtime/game code, and test other titles.
- Bring the thread-dump reader and Maxwell instruction scanner into the repository.

## Design decisions

Upstream releases are ported by hand, allowing component layouts to change independently.
Vulkan replaces OpenGL. Features removed with OpenGL include LSFG, dynamic resolution,
the F7 recorder and F8 full-resolution screenshots.

NVN-level replacement was investigated for Catherine: its loader requests 468 functions
and the game uses about 178. It would replace much of command decoding while retaining
shader translation and host1x support. It remains a possible later rewrite.
