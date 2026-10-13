# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Validate a SASS fragment program translated without GLSL."""

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
    with tempfile.TemporaryDirectory(prefix='snx-direct-shader-') as directory:
        path = Path(directory) / 'fragment.spv'
        vertex = Path(directory) / 'vertex.spv'
        subprocess.run([args.generator, str(path), str(vertex)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(path)], check=True)
        subprocess.run([args.validator, '--target-env', 'vulkan1.3', str(vertex)], check=True)
        result = subprocess.run([args.disassembler, str(path)], check=True, capture_output=True, text=True)
        for expected in (
            'OpEntryPoint Fragment',
            'OriginUpperLeft',
            'NoPerspective',
            'OpCompositeExtract',
            'Location 0',
            'DescriptorSet 1',
            'Binding 0',
            'OpAccessChain',
            'OpFMul',
            'Fma',
            'Exp2',
            'NoContraction',
            'OpImageSampleImplicitLod',
            'OpImageSampleDrefImplicitLod',
            'OpFUnordLessThan',
            'OpSelectionMerge',
            'OpBranchConditional',
        ):
            if expected not in result.stdout:
                raise AssertionError(f'missing {expected} in direct SASS shader')
        vs = subprocess.run([args.disassembler, str(vertex)], check=True, capture_output=True, text=True).stdout
        for expected in (
            'OpEntryPoint Vertex',
            'BuiltIn Position',
            'BuiltIn PointSize',
            'Invariant',
            'OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_1',
        ):
            if expected not in vs:
                raise AssertionError(f'missing {expected} in direct vertex shader')


if __name__ == '__main__':
    main()
