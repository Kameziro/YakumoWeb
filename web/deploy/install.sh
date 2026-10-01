#!/usr/bin/env bash
# Installs the web port with ad hoc play on an Ubuntu or Debian server with
# Nginx, Docker and Node, from a bundle made by web/deploy/bundle.sh:
#
#   sudo DOMAIN=yakumo.example.com ./install.sh
#
# - the page in /var/www/yakumo (Yakumo.*, fonts/ and game/ when the bundle has them)
# - the ad hoc server (Yakumo --adhoc-server) in a Docker container, TCP 27312
#   and 27313 on 127.0.0.1 only; ADHOC_PUBLIC=1 opens them to desktop players
# - the gateway (web/adhoc-gateway) as the systemd service yakumo-adhoc-gateway
# - an Nginx site for $DOMAIN with HTTPS (Let's Encrypt) and a password,
#   unless Nginx already serves $DOMAIN: then it prints what to add instead
#
# Run it again to update; the password file and certificate are kept.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
: "${DOMAIN:?set DOMAIN to the site's host name}"
web_root=/var/www/yakumo
password_file=/etc/nginx/yakumo.htpasswd
site=/etc/nginx/sites-available/yakumo
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }

echo "== ad hoc server"
gunzip -c "$here/yakumo-adhoc-server.tar.gz" | docker load
bind=127.0.0.1
[ "${ADHOC_PUBLIC:-0}" = 1 ] && bind=0.0.0.0
docker rm -f yakumo-adhoc-server > /dev/null 2>&1 || true
docker run -d --name yakumo-adhoc-server --restart unless-stopped \
    -p "$bind:27312:27312" -p "$bind:27313:27313" yakumo-adhoc-server > /dev/null

echo "== gateway"
mkdir -p /opt/yakumo/adhoc-gateway
cp "$here/adhoc-gateway/gateway.mjs" "$here/adhoc-gateway/package.json" "$here/adhoc-gateway/package-lock.json" \
    /opt/yakumo/adhoc-gateway/
(cd /opt/yakumo/adhoc-gateway && npm ci --omit=dev --no-audit --no-fund --silent)
cp "$here/yakumo-adhoc-gateway.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable yakumo-adhoc-gateway > /dev/null
systemctl restart yakumo-adhoc-gateway

echo "== page"
mkdir -p "$web_root"
cp "$here"/www/Yakumo.html "$here"/www/Yakumo.js "$here"/www/Yakumo.wasm "$web_root"/
for folder in fonts game; do
    [ -d "$here/www/$folder" ] && cp -r "$here/www/$folder" "$web_root"/
done
chmod -R a+rX "$web_root"

echo "== nginx"
if [ ! -f "$password_file" ]; then
    password="$(openssl rand -base64 12)"
    printf 'hunter:%s\n' "$(openssl passwd -apr1 "$password")" > "$password_file"
    chgrp www-data "$password_file" && chmod 640 "$password_file"
    echo "Created $password_file: user hunter, password $password"
fi
others="$(grep -rlE "server_name[^;]*[[:space:]]$DOMAIN[[:space:];]" /etc/nginx/sites-enabled/ /etc/nginx/conf.d/ 2>/dev/null \
    | grep -v "^/etc/nginx/sites-enabled/yakumo$" || true)"
if [ -n "$others" ]; then
    echo "Nginx already serves $DOMAIN in: $others"
    echo "Left alone. Add to its HTTPS server block the headers and locations of web/nginx.conf.example"
    echo "(root $web_root, the password, the isolation headers and location /adhoc/), then: nginx -t && systemctl reload nginx"
    exit 0
fi
if [ ! -d "/etc/letsencrypt/live/$DOMAIN" ]; then
    command -v certbot > /dev/null || apt-get install -y certbot > /dev/null
    mkdir -p /var/www/yakumo-acme
    cat > "$site" <<EOF
server {
    listen 80;
    listen [::]:80;
    server_name $DOMAIN;
    location /.well-known/acme-challenge/ { root /var/www/yakumo-acme; }
}
EOF
    ln -sf "$site" /etc/nginx/sites-enabled/yakumo
    nginx -t && systemctl reload nginx
    certbot certonly --webroot -w /var/www/yakumo-acme -d "$DOMAIN" --non-interactive --agree-tos \
        --register-unsafely-without-email
fi
sed "s/yakumo.example.com/$DOMAIN/g" "$here/nginx.conf.example" > "$site"
ln -sf "$site" /etc/nginx/sites-enabled/yakumo
nginx -t && systemctl reload nginx
echo "Done: https://$DOMAIN/"
