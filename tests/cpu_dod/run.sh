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
    "$root/tests/cpu_dod/emit.c" "$root/src/common/cpu/aarch64/a64.c" \
    "$root/src/common/cpu/aarch64/a64_emit.c" -o "$work/emit"
# checked and normal carry the generated interpreter's taken-branch lookup with its fallback: c->rt->miss, as
# snx_lookup answers for a pc without code (SNX_ABI 10)
for row in \
  'step 189a474f57ca545fb73cda63c5da59385dacd52d124fc08a9064a6e752e387cc' \
  'checked ee28b6057b5640d77113122473a8035c4c4d5d256a0e002a8fb66ccbd3413fc2' \
  'normal 166f0f7522222680242324a5043ab9982810f74fc1d7a80d8cec6ea09c1ed5ca'; do
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
    "$root/src/common/cpu/aarch64/a64_emit.c" -o "$work/rtlib_gen"
"$work/rtlib_gen" interp "$work/rtlib_interp.inc"
"$CC" -std=gnu11 -O0 -fsyntax-only -w -I "$work" -I "$root/src" \
    "$root/src/common/cpu/recompiler/llvm/rtlib.c"
echo 'PASS generated CPU runtime C syntax'
