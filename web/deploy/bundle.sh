#!/usr/bin/env bash
# Makes the bundle web/deploy/install.sh installs, in build/deploy/yakumo-deploy
# and build/deploy/yakumo-deploy.tar:
#
#   web/deploy/bundle.sh [linux-build] [web-files]
#
# linux-build: the out/ build whose bin/ holds a Linux Yakumo for the ad hoc
# server, on the web/docker volume (default ci-linux). web-files: the page's
# folder (default build/web): Yakumo.html, .js, .wasm and, when present,
# fonts/ and game/. game/ is your own disc's data: keep the site private.
#
# Then copy the tar to the server, unpack it, and run install.sh as root.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
linux_build="${1:-ci-linux}"
web_files="${2:-$repo/build/web}"
out="$repo/build/deploy/yakumo-deploy"
export MSYS_NO_PATHCONV=1

rm -rf "$out"
mkdir -p "$out/www" "$out/adhoc-gateway" "$repo/build/deploy/server-bin"

# The Linux build lives on the web/docker volume: copy its bin/ out to build a
# server image from it.
bin_host="$repo/build/deploy/server-bin"
command -v cygpath > /dev/null && bin_host="$(cygpath -w "$bin_host")"
docker run --rm -v yakumo-out:/o -v "$bin_host:/dest" yakumo-web-build \
    bash -c "cp /o/$linux_build/bin/Yakumo /dest/ && rm -rf /dest/lib && cp -r /o/$linux_build/bin/lib /dest/"
dockerfile="$here/Dockerfile.adhoc-server"
context="$repo/build/deploy/server-bin"
if command -v cygpath > /dev/null; then
    dockerfile="$(cygpath -w "$dockerfile")"
    context="$(cygpath -w "$context")"
fi
docker build -q -f "$dockerfile" -t yakumo-adhoc-server "$context"
docker save yakumo-adhoc-server | gzip > "$out/yakumo-adhoc-server.tar.gz"

cp "$repo/web/adhoc-gateway/gateway.mjs" "$repo/web/adhoc-gateway/package.json" \
    "$repo/web/adhoc-gateway/package-lock.json" "$out/adhoc-gateway/"
cp "$here/install.sh" "$here/yakumo-adhoc-gateway.service" "$repo/web/nginx.conf.example" "$out/"
cp "$web_files"/Yakumo.html "$web_files"/Yakumo.js "$web_files"/Yakumo.wasm "$out/www/"
for folder in fonts game; do
    [ -d "$web_files/$folder" ] && cp -r "$web_files/$folder" "$out/www/"
done
(cd "$repo/build/deploy" && tar -cf yakumo-deploy.tar yakumo-deploy)
du -sh "$repo/build/deploy/yakumo-deploy.tar"
