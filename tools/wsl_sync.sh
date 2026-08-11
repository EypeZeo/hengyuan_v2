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
#
# Parallel-worktree usage: this repo's parallel-development protocol (see
# CLAUDE.md / docs/SPEC_INVARIANTS.md's Round E Slice 2 entries) runs
# independent git worktrees side by side (e.g. D:\My_Projects\hengyuan_v2_
# slice2a). Each needs its OWN native-filesystem WSL2 copy -- syncing two
# worktrees into the same DST would silently clobber whichever synced second.
# Override the defaults with positional args or env vars to target one:
#   bash tools/wsl_sync.sh /mnt/d/My_Projects/hengyuan_v2_slice2a ~/repos/hengyuan_v2_slice2a
#   HY_SYNC_SRC=/mnt/d/My_Projects/hengyuan_v2_slice2a HY_SYNC_DST=~/repos/hengyuan_v2_slice2a bash tools/wsl_sync.sh

set -euo pipefail

SRC="${1:-${HY_SYNC_SRC:-/mnt/d/My_Projects/hengyuan_v2/}}"
DST="${2:-${HY_SYNC_DST:-${HOME}/repos/hengyuan_v2/}}"
# rsync treats a trailing slash on SRC as "copy contents of", not "copy the
# directory itself" -- enforce it regardless of how the override was spelled,
# so a caller-supplied path without one doesn't silently change this script's
# behavior (e.g. nesting hengyuan_v2_slice2a/ one level too deep in DST).
[[ "${SRC}" == */ ]] || SRC="${SRC}/"
[[ "${DST}" == */ ]] || DST="${DST}/"

mkdir -p "${DST}"

rsync -a --delete \
    --exclude='.git/' \
    --exclude='native/build*/' \
    --exclude='formal/tools/' \
    "${SRC}" "${DST}"

echo "Synced ${SRC} -> ${DST}"
