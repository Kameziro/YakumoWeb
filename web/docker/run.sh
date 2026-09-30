#!/usr/bin/env bash
# Build the web build image and open a shell in it, or run a command in it.
#
#   web/docker/run.sh                     # interactive shell in /src
#   web/docker/run.sh <command...>        # run one command, then exit
#
# The checkout is mounted at /src. Build trees (/src/out) and the compiler
# cache live on named Docker volumes: they are much faster than a bind mount
# on Windows and macOS, and they keep Linux objects out of the host checkout.
# The per-user data directory (EBOOT.ELF, settings, saves) is a volume too.
#
# Set YAKUMO_ISO to your disc image to mount it read-only at /iso/game.iso.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
image=yakumo-web-build

# Git Bash rewrites paths that look like Unix paths; Docker needs them as written.
export MSYS_NO_PATHCONV=1

if command -v cygpath > /dev/null; then
    repo_host="$(cygpath -w "$repo")"
    here_host="$(cygpath -w "$here")"
else
    repo_host="$repo"
    here_host="$here"
fi

docker build -t "$image" "$here_host"

args=(--rm -v "$repo_host:/src" -v yakumo-out:/src/out -v yakumo-ccache:/ccache
      -v yakumo-data:/root/.local/share/Yakumo -w /src)
if [ -n "${YAKUMO_ISO:-}" ]; then
    iso_host="$YAKUMO_ISO"
    command -v cygpath > /dev/null && iso_host="$(cygpath -w "$YAKUMO_ISO")"
    args+=(-v "$iso_host:/iso/game.iso:ro")
fi

if [ $# -eq 0 ]; then
    exec docker run -it "${args[@]}" "$image" bash
else
    exec docker run "${args[@]}" "$image" bash -lc "ulimit -s 65536; . /opt/emsdk/emsdk_env.sh > /dev/null 2>&1; $*"
fi
