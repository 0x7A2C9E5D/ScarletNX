#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Makes dist/ScarletNX-linux-<arch>.AppImage, a runtime that runs on any Linux, with pkgforge's quick-sharun
# (https://github.com/pkgforge-dev/Anylinux-AppImages): it bundles the runtime with every library it loads, SDL3, the
# Vulkan loader and Mesa's Vulkan drivers included. CI runs it in pkgforge's Arch container after `cmake --install`
# into /usr (.github/workflows/build.yml), and quick-sharun has to be on PATH (pkgforge-dev/anylinux-setup-action puts
# it there).
#
# Anylinux-AppImages is this packaging's reference: its defaults (quick-sharun's, its FAQ.md) come before a tool, a
# download or a step of our own. The official AppImage documentation and the tools built on it (linuxdeploy,
# appimagekit, appimage-builder) describe another, older way and do not apply here.
#
# A copy named after a game works like the bare runtime: games/MyGame.AppImage (or games/MyGame) loads
# games/MyGame_code.so, since the runtime takes its name and folder from $APPIMAGE.
#
# An install of the Linux runtime alone (a big-endian host's: no scarletnx, only scarletnx-gnuabi) makes an AppImage
# of it: it runs Linux AArch64 programs (`ScarletNX-linux-ppc64.AppImage ./program`), and its checks run one.
#
# With SYSROOT and ARCH set, the AppImage is made for another architecture on this machine: the runtime was
# cross-compiled and installed into the sysroot (an Arch root of that architecture), and the libraries and licenses
# to bundle come from there. CI makes the ppc64le, riscv64 and loongarch64 ones this way (.github/workflows/build.yml,
# appimage-cross). quick-sharun runs natively, and the appimagetool it fetches is the target's: the kernel's binfmt
# handler for the target runs it, as it runs the plain ELF programs the AppImage starts, so it has to be registered.
# appimagetool takes a mkdwarfs on PATH before the target's it would fetch, and a DwarFS image is the same whichever
# machine makes it: CI puts this machine's there, as the target's compresses many times slower under qemu. The
# target's loader behind `ldd` runs through qemu-user, the target's files are stripped with llvm-strip, and the checks
# at the end run the AppImage itself through qemu-$ARCH: that handler never takes an AppImage, since its runtime's ELF
# header carries the AppImage magic ("AI\x02" at byte 8) where qemu's mask wants zeros.
set -euo pipefail

SYSROOT=${SYSROOT:-} # the target's system root; empty packages this machine's own /usr
ARCH=${ARCH:-$(uname -m)}
REPOSITORY=${GITHUB_REPOSITORY:-scarletnx/ScarletNX}
VERSION=${VERSION:-$(git -C "$(dirname "$0")/.." describe --always --dirty 2>/dev/null || echo unknown)}
export ARCH VERSION
# The architecture inside the AppImage: appimagetool picks its runtime by it, and quick-sharun its downloads
export APPIMAGE_ARCH=$ARCH
export OUTPATH=./dist
export OUTNAME=ScarletNX-linux-$ARCH.AppImage
# quick-sharun copies ICON to .DirIcon as well
export ICON=$SYSROOT/usr/share/icons/hicolor/256x256/apps/wimsy.png
# the AppImage's program: the driver, else the Linux runtime when it is all the install has
bin=$SYSROOT/usr/bin
main=scarletnx
if [ ! -e "$bin/scarletnx" ] && [ -e "$bin/scarletnx-gnuabi" ]; then
    main=scarletnx-gnuabi
fi
export DESKTOP=$SYSROOT/usr/share/applications/$main.desktop
# AppImageUpdate finds new builds through the .zsync file next to the AppImage in the latest release
# (scripts/publish-builds.py): a version's in this repository's, a nightly's in the owner's nightly repository
if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
    export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|${REPOSITORY#*/}|latest|*$ARCH.AppImage.zsync"
else
    export UPINFO="gh-releases-zsync|${REPOSITORY%/*}|nightly|latest|*$ARCH.AppImage.zsync"
fi
# The runtime exits at once without a game module, so tracing it for dlopened libraries finds nothing: name them
export STRACE_MODE=0 DEPLOY_SDL=1 DEPLOY_OPENGL=1

# pacman over the sysroot when there is one: its database records the bundled libraries' packages, and -Qo takes a
# path inside the root it is given, which it takes off before looking the file up
pacman_query() {
    if [ -n "$SYSROOT" ]; then
        pacman --root "$SYSROOT" --dbpath "$SYSROOT/var/lib/pacman" "$@"
    else
        pacman "$@"
    fi
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if [ -n "$SYSROOT" ]; then
    if [ "$ARCH" = "$(uname -m)" ]; then
        echo "SYSROOT packages another architecture: set ARCH to the sysroot's (uname -m names it in the sysroot)" >&2
        exit 2
    fi
    export LIB_DIR=$SYSROOT/usr/lib
    # quick-sharun takes the architecture of what it bundles from `uname -m`, and the libraries a binary loads from
    # `ldd`, whose loader is this machine's and refuses the target's binaries: answer both for the target. The
    # sysroot's own loader lists the libraries, run by qemu-user with the sysroot as its root
    shim=$tmp/shim
    mkdir -p "$shim"
    cat >"$shim/uname" <<EOF
#!/bin/sh
[ "\${1:-}" = -m ] && echo $ARCH && exit 0
exec /usr/bin/uname "\$@"
EOF
    # The target's loader, its symlinks followed inside the sysroot (an absolute one would land on this machine)
    interp=$(patchelf --print-interpreter "$bin/$main")
    for _ in 1 2 3 4 5 6 7 8; do
        [ -L "$SYSROOT$interp" ] || break
        link=$(readlink "$SYSROOT$interp")
        case $link in
            /*) interp=$link ;;
            *) interp=$(dirname "$interp")/$link ;;
        esac
    done
    machine=$(od -An -tu2 -j18 -N2 "$bin/$main" | tr -d ' ')
    # A target program's libraries as its loader finds them, each path made the sysroot's so quick-sharun copies the
    # target's library and not this machine's; a loader that fails says so. Anything else is this machine's ldd's
    cat >"$shim/ldd" <<EOF
#!/bin/sh
[ "\$(od -An -tu2 -j18 -N2 "\$1" 2>/dev/null | tr -d ' ')" = $machine ] || exec /usr/bin/ldd "\$@"
out=\$(qemu-$ARCH -L "$SYSROOT" "$SYSROOT$interp" --list "\$1" 2>&1) || {
    printf 'ldd: the $ARCH loader could not list %s:\n%s\n' "\$1" "\$out" >&2
    exit 1
}
printf '%s\n' "\$out" | awk -v root="$SYSROOT" '{ for (i = 1; i <= NF; i++) if (\$i ~ /^\//) \$i = root \$i; print }'
EOF
    # quick-sharun strips what it bundles with `strip`, and this machine's knows its own architecture only: it fails on
    # every file of the target, which quick-sharun lets pass, and the AppImage would carry their symbols and debug
    # information. llvm-strip takes any
    llvm_strip=$(command -v llvm-strip) || {
        echo "llvm-strip strips the $ARCH files, and it is not on PATH" >&2
        exit 1
    }
    printf '#!/bin/sh\nexec %s "$@"\n' "$llvm_strip" >"$shim/strip"
    chmod +x "$shim/uname" "$shim/ldd" "$shim/strip"
    PATH=$shim:$PATH
    export PATH
    # without the runtime's own libraries from the sysroot the AppImage would start nowhere: stop here, saying why
    listed=$(ldd "$bin/$main") || exit 1
    if ! grep -qF "$SYSROOT/usr/lib/" <<<"$listed"; then
        echo "ldd listed none of $bin/$main's libraries in $SYSROOT:" >&2
        echo "$listed" >&2
        exit 1
    fi
    echo "$listed"
fi

# The runtime stays the AppImage's program (the desktop entry's Exec), so a copy named after a game runs that game.
# Started bare it opens its launcher, which runs Switch games with it and the other drivers from beside it
# (scarletnx-3ds, scarletnx-gnuabi)
if [ "$main" = scarletnx ]; then
    quick-sharun "$bin/scarletnx" "$bin/scarletnx-switch" "$bin/scarletnx-3ds" "$bin/scarletnx-gnuabi"
else
    quick-sharun "$bin/scarletnx-gnuabi"
fi

# The licenses travel with the AppImage: the runtime's and those of the libraries built into it (`cmake --install` put
# them in /usr/share/licenses/scarletnx, in the sysroot for a cross build), and those of the Arch packages quick-sharun
# took the bundled libraries from, with a list of those packages, their versions and licenses
appdir=${APPDIR:-$PWD/AppDir}
if [ ! -d "$appdir/lib" ]; then
    echo "quick-sharun left no $appdir/lib: where it puts the bundled libraries has changed" >&2
    exit 1
fi
if [ "$(head -c 8 "$appdir/.DirIcon" | od -An -tx1 | tr -d ' \n')" != 89504e470d0a1a0a ]; then
    echo "$appdir/.DirIcon is not a PNG: quick-sharun no longer takes ICON for it" >&2
    exit 1
fi

# and the vector next to it, the best the desktop entry's Icon= can name
cp "$SYSROOT/usr/share/icons/hicolor/scalable/apps/wimsy.svg" "$appdir/"
mkdir -p "$appdir/share/licenses"
cp -r "$SYSROOT/usr/share/licenses/scarletnx" "$appdir/share/licenses/"
mapfile -t packages < <(cd "$appdir/lib" && find . -name '*.so*' -type f | while read -r lib; do
    pacman_query -Qqo "$SYSROOT/usr/lib/${lib#./}" 2>/dev/null || true
done | sort -u)
if [ -n "$SYSROOT" ] && [ ${#packages[@]} = 0 ]; then
    echo "No bundled library belongs to a package of $SYSROOT's database: the licenses can't be found" >&2
    exit 1
fi
for pkg in "${packages[@]}"; do
    pacman_query -Qi "$pkg" | grep -E '^(Name|Version|Licenses) '
    echo
    if [ -d "$SYSROOT/usr/share/licenses/$pkg" ]; then
        cp -r "$SYSROOT/usr/share/licenses/$pkg" "$appdir/share/licenses/"
    fi
done >"$appdir/share/licenses/bundled-packages.txt"

quick-sharun --make-appimage

# Without a game the runtime says which module it looked for. Under another name, it must look next to the AppImage
# for that name's module, which shows the libraries load and the name and folder come from $APPIMAGE. With --game it
# must look for the module that names
probe=$tmp/probe
mkdir -p "$probe"
cp "$OUTPATH/$OUTNAME" "$probe/Probe.AppImage"
launch=()
if [ -n "$SYSROOT" ]; then
    launch=("$(command -v "qemu-$ARCH")") || {
        echo "qemu-$ARCH runs the $ARCH AppImage's checks, and it is not on PATH" >&2
        exit 1
    }
fi
# The Linux runtime alone: it runs an AArch64 program, which prints its line and exits with 42
if [ "$main" = scarletnx-gnuabi ]; then
    "$(dirname "$0")/aarch64-smoke.sh" "$probe/smoke"
    status=0
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 "${launch[@]}" ./Probe.AppImage ./smoke 2>&1) || status=$?
    if [ "$status" != 42 ] || ! grep -qx 'gnuabi smoke' <<<"$out"; then
        echo "The AppImage ran an AArch64 program to exit status $status, not 42 with its line. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
    echo "AppImage checked: it runs Linux AArch64 programs"
    exit 0
fi
check() { # expected module, then the AppImage's arguments
    local want=$1 out
    shift
    out=$(cd "$probe" && APPIMAGE_EXTRACT_AND_RUN=1 "${launch[@]}" ./Probe.AppImage "$@" 2>&1 || true)
    if ! grep -qF "$want" <<<"$out"; then
        echo "The AppImage did not look for $want. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
}
# -H: started bare, it would open the launcher instead
check "$probe/Probe_code.so" -H
check "$probe/games/Other_code.so" --game="$probe/games/Other"
# Started bare it opens the launcher, which needs everything it runs beside it (the launcher itself can't be run
# here: without a display SDL may fall back to an offscreen one and keep it open)
for program in scarletnx-3ds scarletnx-gnuabi; do
    if [ ! -e "$appdir/bin/$program" ]; then
        echo "The AppImage has no bin/$program, which the launcher runs" >&2
        exit 1
    fi
    # a name of scarletnx's where it is that driver too (SNX_MULTICALL): sharun has to start it under its own name
    out=$("${launch[@]}" "$appdir/bin/$program" --help 2>&1 || true)
    if ! grep -q "^Usage: $program" <<<"$out"; then
        echo "The AppImage's bin/$program --help is not $program's. It printed:" >&2
        echo "$out" >&2
        exit 1
    fi
done
echo "AppImage checked: it looks for its game module next to itself or where --game says, and has the launcher's drivers"
