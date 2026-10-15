# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Stage-1 CLI contract; no firmware, keys or game assets required."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    driver = pathlib.Path(sys.argv[1]).resolve()

    def run(args, status, contains, cwd=None):
        p = subprocess.run([str(driver), *args], capture_output=True, text=True, cwd=cwd, timeout=10)
        output = p.stdout + p.stderr
        assert p.returncode == status, (args, p.returncode, output)
        assert contains in output, (args, output)
        assert "ScarletNX - " not in output, output  # no false launch/completion banner

    run(["--help"], 0, "Stage 1")
    run([], 2, "Usage:")
    run(["--model"], 2, "requires old or new")
    run(["--model", "other", "--board-info"], 2, "requires old or new")
    run(["--board-info"], 0, "Old 3DS\nARM11 cores: 2\nBase clock: 268111856 Hz\nFCRAM: 128 MiB")
    run(["--model", "new", "--board-info"], 0, "New 3DS\nARM11 cores: 4")
    run(["--board-info", "game.cia"], 2, "Usage:")
    run(["--backend=gcc"], 2, "unexpected argument")
    with tempfile.TemporaryDirectory(prefix="snx-3ds-") as temp:
        root = pathlib.Path(temp)
        for name in ["game with spaces;literal.CIA", "game.nsp", "--game.cia"]:
            (root / name).write_bytes(b"not a real CIA")
        run([str(root / "game with spaces;literal.CIA")], 3, "not implemented (stage 2)")
        run(["--model", "new", "--", "--game.cia"], 3, "not implemented", cwd=root)
        run([str(root / "game.nsp")], 2, "expected a readable .cia")
        run([str(root / "missing.cia")], 2, "expected a readable .cia")
        run([str(root)], 2, "expected a readable .cia")
        run([str(root / "game.nsp"), "second.cia"], 2, "unexpected argument")
        assert sorted(p.name for p in root.iterdir()) == ["--game.cia", "game with spaces;literal.CIA", "game.nsp"]
    print("3DS driver CLI, unsupported-stage diagnostics and no output side effects passed")


if __name__ == "__main__":
    main()
