#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Runs a game with the Tracy profiler: the one command for a profiled run. The trace lands beside the game and opens
# in the profiler when the run ends.
#
#   scripts/tracy.sh TITLE_ID [--gui] [--seconds N] [--runtime PATH] [-- RUNTIME-ARGS...]
#
# The runtime must be built with the profiler (cmake --preset tracy && cmake --build --preset tracy).
# Without --gui the run is recorded to a file, and waits for the capture to connect before the game starts;
# with it the client comes up as the game starts and the GUI attaches whenever it is started.
# Anything after -- goes to the runtime as it is, so a scripted run (--script=, --shot=) can be profiled too.
set -euo pipefail

id=
gui=0
seconds=
runtime=
extra=()
while [ $# -gt 0 ]; do
    case $1 in
        --gui) gui=1; shift ;;
        --seconds=*) seconds=${1#*=}; shift ;;
        --seconds) seconds=$2; shift 2 ;;
        --runtime=*) runtime=${1#*=}; shift ;;
        --runtime) runtime=$2; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        -h | --help) sed -n '2,12p' "$0"; exit 0 ;;
        -*) echo "$0: unknown option $1: put the runtime's own options after --" >&2; exit 2 ;;
        *) id=$1; shift ;;
    esac
done
if [ -z "$id" ]; then
    echo "usage: $0 TITLE_ID [--gui] [--seconds N] [--runtime PATH] [-- RUNTIME-ARGS...]" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "$runtime" ]; then runtime=$root/build/tracy/scarletnx; fi
if [ ! -x "$runtime" ]; then
    echo "$0: no runtime at $runtime: build it with 'cmake --preset tracy && cmake --build --preset tracy'" >&2
    exit 2
fi
data=${XDG_DATA_HOME:-$HOME/.local/share}/scarletnx/games
if [ ! -d "$data/$id" ]; then
    echo "$0: no game $id in $data" >&2
    exit 2
fi

if [ $gui -eq 1 ]; then
    echo "tracy: the game runs; connect the GUI whenever you like: tracy-profiler -a 127.0.0.1 -p 8086"
    exec "$runtime" --game="$data/$id" --tracy ${seconds:+--seconds=$seconds} ${extra[@]+"${extra[@]}"}
else
    exec "$runtime" --game="$data/$id" --tracy-capture ${seconds:+--seconds=$seconds} ${extra[@]+"${extra[@]}"}
fi
