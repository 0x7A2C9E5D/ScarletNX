# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Check a direct-emitted module with system SPIRV-Tools."""

import argparse
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True)
    parser.add_argument('--validator', required=True)
    parser.add_argument('--disassembler', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='snx-spirv-emitter-') as directory:
        path = Path(directory) / 'compute.spv'
        subprocess.run([args.generator, str(path)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(path)], check=True)
        result = subprocess.run([args.disassembler, str(path)], check=True, capture_output=True, text=True)
        disassembly = result.stdout
        for expected in (
            'OpEntryPoint GLCompute',
            'OpExecutionMode %main LocalSize 1 1 1',
            'OpTypeVector %float 4',
            'OpConstantComposite',
            'OpAccessChain',
            'OpIAdd',
            'OpStore',
            'OpReturn',
        ):
            if expected not in disassembly:
                raise AssertionError(f'missing {expected} in direct-emitted SPIR-V')


if __name__ == '__main__':
    main()
