#!/usr/bin/env bash
# Local bare mirrors of the two FetchContent dependencies, for WSL2 runs.
#
# Why: native/CMakeLists.txt's FetchContent clones simdjson and googletest from GitHub at configure time, once per build directory (build-linux-none,
# -asan, -tsan, ...). A flaky connection ("curl 92: HTTP/2 stream was not closed cleanly") kills the configure, and a failing FetchContent retries for
# 10 to 20 minutes before it gives up: on 2026-10-20 that cost the first WSL run of a slice all three tiers (none, address, thread), none of it caused
# by the code. With the mirrors in place tools/wsl_verify.sh points git at them for its own process (GIT_CONFIG_* environment variables, see --env; no
# global git config is touched), so a configure needs no network at all.
#
# The URL and the tag of each dependency are READ from native/CMakeLists.txt (the FetchContent_Declare blocks), so the mirrors can never silently
# disagree with CI; a bumped pin simply makes the mirror stale (wsl_verify.sh then falls back to the network, as before) until this script is run again.
#
# Usage (inside WSL2):
#   bash tools/wsl_deps_mirror.sh           create or refresh what is missing
#   bash tools/wsl_deps_mirror.sh --check   exit 0 if both mirrors hold their pinned tag, 1 otherwise; changes nothing
#   bash tools/wsl_deps_mirror.sh --env     print the `export GIT_CONFIG_*` lines that make git use the mirrors (nothing at all unless BOTH mirrors are
#                                           complete); `eval "$(bash tools/wsl_deps_mirror.sh --env)"`
# Mirrors live in ${HY_DEPS_MIRROR:-$HOME/.cache/hy_deps}/<name>.git. Creating one tries, in order: a populated checkout left by an earlier build
# directory under ~/repos (no network), then the network over HTTP/1.1 (the flake is an HTTP/2 stream cancel), four tries.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKELISTS="${REPO_ROOT}/native/CMakeLists.txt"
MIRROR_ROOT="${HY_DEPS_MIRROR:-${HOME}/.cache/hy_deps}"
DEPS=(simdjson googletest)

mode="make"
case "${1:-}" in
    "") ;;
    --check) mode="check" ;;
    --env) mode="env" ;;
    *) echo "usage: $0 [--check|--env]" >&2; exit 2 ;;
esac

# pin <name> <GIT_REPOSITORY|GIT_TAG>: that field of the FetchContent_Declare(<name> ...) block. A copy synced from a Windows working tree has CRLF line
# ends (tools/wsl_sync.sh copies the bytes as they are): the first rule strips the CR, or every field would end in one and no name would ever match.
pin() {
    awk -v name="$1" -v key="$2" '
        { sub(/\r$/, "") }
        /FetchContent_Declare\(/ { in_decl = 1; seen = 0; next }
        in_decl && !seen && $1 == name { seen = 1; next }
        in_decl && seen && $1 == key { print $2; exit }
        in_decl && /\)/ { in_decl = 0 }
    ' "${CMAKELISTS}"
}

has_tag() { git -C "$1" rev-parse --verify -q "refs/tags/$2^{commit}" >/dev/null 2>&1; }

missing=0
env_lines=()
env_n="${GIT_CONFIG_COUNT:-0}"
for name in "${DEPS[@]}"; do
    url="$(pin "${name}" GIT_REPOSITORY)"
    tag="$(pin "${name}" GIT_TAG)"
    if [[ -z "${url}" || -z "${tag}" ]]; then
        echo "cannot read the pin of ${name} from ${CMAKELISTS}" >&2
        exit 2
    fi
    mirror="${MIRROR_ROOT}/${name}.git"

    if has_tag "${mirror}" "${tag}"; then
        [[ "${mode}" == "env" ]] || echo "${name}: ${tag} already mirrored (${mirror})"
        env_lines+=("export GIT_CONFIG_KEY_${env_n}='url.file://${mirror}.insteadOf'" "export GIT_CONFIG_VALUE_${env_n}='${url}'")
        env_n=$((env_n + 1))
        continue
    fi
    missing=1
    if [[ "${mode}" != "make" ]]; then
        [[ "${mode}" == "env" ]] || echo "${name}: ${tag} NOT mirrored"
        continue
    fi

    mkdir -p "${MIRROR_ROOT}"
    rm -rf "${mirror}"
    done_one=0
    while IFS= read -r d; do
        if has_tag "${d}" "${tag}" && git clone --bare --quiet "${d}" "${mirror}"; then
            echo "${name}: mirrored from ${d}"
            done_one=1
            break
        fi
        rm -rf "${mirror}"
    done < <(find "${HOME}/repos" -maxdepth 7 -type d -name "${name}-src" 2>/dev/null)
    if [[ ${done_one} -eq 0 ]]; then
        for try in 1 2 3 4; do
            if git -c http.version=HTTP/1.1 clone --bare --quiet "${url}" "${mirror}"; then
                echo "${name}: cloned over HTTP/1.1 (try ${try})"
                done_one=1
                break
            fi
            rm -rf "${mirror}"
            sleep 5
        done
    fi
    if [[ ${done_one} -eq 0 ]]; then
        echo "${name}: could not be mirrored" >&2
        exit 3
    fi
    if ! has_tag "${mirror}" "${tag}"; then
        echo "${name}: the mirror does not hold ${tag}" >&2
        exit 4
    fi
    missing=0
done

if [[ "${mode}" == "env" ]]; then
    # all or nothing: a half-mirrored set would send one dependency to the network and hide that behind a "mirrors in use" message
    if [[ ${missing} -eq 0 ]]; then
        env_lines+=("export GIT_CONFIG_KEY_${env_n}='http.version'" "export GIT_CONFIG_VALUE_${env_n}='HTTP/1.1'")
        env_n=$((env_n + 1))
        env_lines+=("export GIT_CONFIG_COUNT=${env_n}")
        printf '%s\n' "${env_lines[@]}"
    fi
    exit 0
fi

if [[ "${mode}" == "check" ]]; then
    exit "${missing}"
fi
echo "mirrors ready in ${MIRROR_ROOT}"
