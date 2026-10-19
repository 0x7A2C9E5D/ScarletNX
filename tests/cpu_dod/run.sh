#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
: "${CC:=clang}"

"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -DSNX_TYPED -I "$root/src" \
    "$root/tests/cpu_dod/reference.c" "$root/src/common/cpu/aarch64/a64.c" -o "$work/reference"
expected='52 families 1585 cases 21c6c7d024b52ced'
actual="$("$work/reference")"
[[ "$actual" == "$expected" ]] || { echo "FAIL decoder fingerprint: $actual" >&2; exit 1; }
echo "PASS original decoder fingerprint: $actual"

"$CC" -std=gnu11 -O2 -Wall -Wextra -Werror -I "$root/src" \
    "$root/tests/cpu_dod/emit.c" "$root/src/common/cpu/aarch64/a64.c" -o "$work/emit"
for row in \
  'step 189a474f57ca545fb73cda63c5da59385dacd52d124fc08a9064a6e752e387cc' \
  'checked 7a617f98712a02df42a1811ff388bffaf9f9a64dc3fe8d7186e189012ef29621' \
  'normal 0f4ec7dc98c5a11b79a3c7c91e800ad2849caa62a09e00312a895cd509ed3052'; do
    read -r mode digest <<< "$row"
    actual="$("$work/emit" "$mode" | sha256sum | cut -d' ' -f1)"
    [[ "$actual" == "$digest" ]] || { echo "FAIL $mode generated C: $actual" >&2; exit 1; }
    echo "PASS $mode generated C byte-for-byte baseline fingerprint"
done

python3 "$root/tests/llvm_dod/test_decoder.py" "$work/decoder.c"
"$CC" -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -I "$root/src" "$work/decoder.c" "$root/src/common/cpu/aarch64/a64.c" -o "$work/decoder"
"$work/decoder"

# Runtime C still compiles with the generated interpreter and original ABI.
"$CC" -std=gnu11 -O0 -I "$root/src" \
    "$root/src/common/cpu/recompiler/llvm/rtlib_gen.c" "$root/src/common/cpu/aarch64/a64.c" \
    -o "$work/rtlib_gen"
"$work/rtlib_gen" interp "$work/rtlib_interp.inc"
"$CC" -std=gnu11 -O0 -fsyntax-only -w -I "$work" -I "$root/src" \
    "$root/src/common/cpu/recompiler/llvm/rtlib.c"
echo 'PASS generated CPU runtime C syntax'
