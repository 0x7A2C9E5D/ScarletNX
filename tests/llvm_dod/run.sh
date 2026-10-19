#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 "$root/tests/llvm_dod/test_decoder.py" "$work/decoder.c"
: "${CC:=clang}"
"$CC" -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -pthread -I "$root/src" "$work/decoder.c" "$root/src/common/cpu/aarch64/a64.c" -o "$work/decoder"
"$work/decoder"
# Keep the full LLVM backend's source/dependency map explicit.
python3 - "$root" <<'PY'
from pathlib import Path
import re,sys
root=Path(sys.argv[1]); p=root/'src/common/cpu/recompiler/llvm'
cm=(p/'CMakeLists.txt').read_text()
for name in ('ir.c','ir_state.c','ir_emit.c','ir_helpers.c','ir_a64.c','ir_fp.c','ir_simd.c','ir_codegen.c'):
 assert re.search(r'\b'+re.escape(name)+r'\b',cm), f'missing CMake source: {name}'
s=(p/'ir_simd.c').read_text()
for name in ('ir_neon_memory.inc','ir_neon_crypto.inc','ir_neon_arithmetic.inc','ir_neon_dispatch.inc'):
 assert f'#include "{name}"' in s, f'missing NEON source partition: {name}'
 assert (p/name).exists()
print('PASS module dependency map: 9 LLVM IR .c units and 4 NEON source partitions')
PY

# The new runtime calls SVCs through ABI 9's indexed table. Check the actual
# emitter with the upstream CPU layout, LLVM verifier, and native execution.
: "${LLVM_LIBRARY:=$(llvm-config --libdir 2>/dev/null)/libLLVM.so}"
if [[ -f "$LLVM_LIBRARY" ]]; then
    python3 "$root/tests/llvm_dod/test_svc_abi9.py" "$work/svc_abi9.c"
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$root/src" \
        "$work/svc_abi9.c" "$LLVM_LIBRARY" -o "$work/svc_abi9"
    "$work/svc_abi9"
    python3 "$root/tests/llvm_dod/test_fp_guards.py" "$work/fp_guards.c"
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror "$work/fp_guards.c" "$LLVM_LIBRARY" -o "$work/fp_guards"
    "$work/fp_guards"
    python3 "$root/tests/llvm_dod/test_neon_phi.py" "$work/neon_phi.c"
    "$CC" -std=gnu11 -O2 -Wall -Wextra -Werror "$work/neon_phi.c" "$LLVM_LIBRARY" -o "$work/neon_phi"
    "$work/neon_phi"
else
    echo "SKIP ABI 9 LLVM execution check: LLVM_LIBRARY unavailable"
fi
