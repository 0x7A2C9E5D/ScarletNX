#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
repo="$(cd ../.. && pwd)"
: "${CC:=clang}"
: "${LLVM_LIBRARY:=$(llvm-config --libdir 2>/dev/null)/libLLVM.so}"
if [[ ! -f "$LLVM_LIBRARY" ]]; then
    echo "Set LLVM_LIBRARY to a shared LLVM library" >&2
    exit 2
fi
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
# ISA-level decoder checks live in tests/cpu_dod; LLVM no longer owns a case lookup.
python3 create_llvm_harness.py
python3 extend_neon_harness.py
"$CC" -std=gnu11 -O2 -Werror llvm_neon_real.c "$LLVM_LIBRARY" -o "$build/neon_test"
"$build/neon_test"
python3 create_cache_harness.py
"$CC" -std=gnu11 -O2 -Wno-unused-function llvm_gpr_cache_test.c "$LLVM_LIBRARY" -o "$build/cache_test"
"$build/cache_test"
