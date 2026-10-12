<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Filesystem

Startup indexes every directory and regular file under `<Game>_data/` and stores its
absolute path. Top-level directories are mounts:

| Guest path | Data-directory path |
| --- | --- |
| `romfs:/file` | `romfs/file` |
| `sdmc:/file` | `sdmc/file` |
| `/file` | `root/file` |

Filesystem service handles restrict paths to their mount. Save services retain
`save/user`, `save/system` and the other existing save-type directories.

Lookups, sizes and directory listings use the tree. Host paths are constructed only
after resolving indexed nodes. Create, rename, delete, write and resize operations
update the tree under one lock. Failed operations preserve existing nodes and record
any successful partial changes. Open handles retain nodes across renames.

The tree also holds emulated files. Missing system archives appear as readonly empty
RomFS files under `system/archives/`. An OS fallback font, when needed, is copied into
`fonts/SystemFallback.ttf`. These files need no persistent host copy.

External changes during a run are unsupported. Restart after editing game data.
Startup rejects links, special files, unreadable directories and paths beyond the
supported limits instead of building an incomplete index. Directory reads advance
in name order, while entry counts reflect current indexed contents.

Run the synthetic tree and service checks without game data:

```bash
python3 tests/run_filesystem_tree.py --cc clang --sanitize
```

On Windows, use `py -3` and pass the Clang executable path to `--cc` if needed.
