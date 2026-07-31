#!/usr/bin/env bash
# Sync the Windows-side working tree into WSL2's native filesystem for building.
#
# Why not build directly on /mnt/d/...: WSL2 reaches Windows drives over the 9p
# protocol, which is materially slower for the kind of workload this repo's build
# does -- FetchContent pulling and compiling simdjson and googletest from source is
# thousands of small file operations. Building on ext4 instead of 9p is the single
# biggest lever on local iteration speed here.
#
# Why rsync instead of git clone/pull: the point of this local verification path is
# "catch what CI would catch, before you push" -- including uncommitted work in
# progress. A git-based sync would only ever see committed history.
#
# Usage: run from inside WSL2 (not from PowerShell):
#   bash tools/wsl_sync.sh
# or non-interactively from Windows:
#   wsl -d Ubuntu-24.04 -- bash /mnt/d/My_Projects/hengyuan_v2/tools/wsl_sync.sh

set -euo pipefail

SRC="/mnt/d/My_Projects/hengyuan_v2/"
DST="${HOME}/repos/hengyuan_v2/"

mkdir -p "${DST}"

rsync -a --delete \
    --exclude='.git/' \
    --exclude='native/build*/' \
    --exclude='formal/tools/' \
    "${SRC}" "${DST}"

echo "Synced ${SRC} -> ${DST}"
