#!/usr/bin/env bash
# ── ESP32 PM Monitor — Oracle Cloud Always Free bootstrap ─────────────────────
# Tested on: Ubuntu 22.04 ARM64 (Ampere A1 — 4 OCPU / 24 GB free)
#            Ubuntu 22.04 x86_64 (AMD micro — 1/8 OCPU / 1 GB free)
#
# Pre-flight checklist (do these before running this script):
#   1. Create an OCI Compute instance with an Always Free shape.
#   2. In the instance's VCN Security List (or NSG) open:
#        Ingress TCP 22   (SSH — already done if you're reading this)
#        Ingress TCP 80   (HTTP — needed for Let's Encrypt challenge)
#        Ingress TCP 443  (HTTPS — dashboard + ESP32 posts)
#   3. Point a domain (or free DuckDNS subdomain) at this instance's public IP.
#   4. (Recommended) Attach a Block Volume, format it, mount it at /data,
#      and add it to /etc/fstab.  Without a block volume, data lives in the
#      instance's boot volume (survives reboots but not re-provisioning).
#   5. Clone / copy this repository onto the instance.
#
# Usage (from esp32-pm-monitor/oracle-cloud/):
#   chmod +x setup.sh && ./setup.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="/data/pm_data"

# ── Helpers ───────────────────────────────────────────────────────────────────
info()  { echo "[setup] $*"; }
die()   { echo "[error] $*" >&2; exit 1; }

# ── Step 1: Install Docker (skip if already present) ──────────────────────────
if ! command -v docker &>/dev/null; then
    info "Installing Docker..."
    curl -fsSL https://get.docker.com | sudo sh
    sudo usermod -aG docker "$USER"
    info "Docker installed."
    info "NOTE: You may need to log out and back in for group membership to take effect."
    info "      If docker commands fail, run: newgrp docker"
else
    info "Docker already installed: $(docker --version)"
fi

# ── Step 2: Open firewall (Ubuntu ufw) ────────────────────────────────────────
if command -v ufw &>/dev/null; then
    info "Configuring ufw..."
    sudo ufw allow 80/tcp  comment "PM Monitor HTTP/ACME"  > /dev/null
    sudo ufw allow 443/tcp comment "PM Monitor HTTPS"      > /dev/null
    # Reload only if ufw is already active (avoids interactive prompt)
    if sudo ufw status | grep -q "Status: active"; then
        sudo ufw reload > /dev/null
        info "ufw reloaded: ports 80 and 443 open"
    else
        info "ufw rules added (ufw not yet active — enable with: sudo ufw enable)"
    fi
fi

# ── Step 3: Prepare persistent data directory ─────────────────────────────────
sudo mkdir -p "$DATA_DIR"
sudo chown "$USER:$USER" "$DATA_DIR"
info "Data directory: $DATA_DIR"

# ── Step 4: Collect configuration interactively ───────────────────────────────
echo ""
echo "─────────────────────────────────────────────────────────────────────────────"
echo " ESP32 PM Monitor — Oracle Cloud Setup"
echo "─────────────────────────────────────────────────────────────────────────────"

read -rp "Domain name (e.g. pm.example.com): " DOMAIN
[[ -n "$DOMAIN" ]] || die "Domain is required"

read -rp "Email for Let's Encrypt expiry notices: " LE_EMAIL
[[ -n "$LE_EMAIL" ]] || die "Email is required"

read -rsp "API key for ESP32 [leave blank to generate]: " API_KEY_IN
echo ""
if [[ -z "$API_KEY_IN" ]]; then
    API_KEY_IN="$(openssl rand -hex 16)"
    info "Generated API_KEY: $API_KEY_IN  (save this for config.h)"
fi

read -rp "Dashboard username [admin]: " DASH_USER_IN
DASH_USER_IN="${DASH_USER_IN:-admin}"

while true; do
    read -rsp "Dashboard password: " DASH_PASS_IN
    echo ""
    read -rsp "Confirm password: " DASH_PASS_CONFIRM
    echo ""
    [[ "$DASH_PASS_IN" == "$DASH_PASS_CONFIRM" ]] && break
    echo "Passwords do not match — try again."
done
[[ -n "$DASH_PASS_IN" ]] || die "Password cannot be empty"

read -rp "Max records to retain [10000]: " MAX_RECORDS_IN
MAX_RECORDS_IN="${MAX_RECORDS_IN:-10000}"

# ── Step 5: Write .env ────────────────────────────────────────────────────────
cat > "$SCRIPT_DIR/.env" <<EOF
API_KEY=${API_KEY_IN}
DASH_USER=${DASH_USER_IN}
DASH_PASS=${DASH_PASS_IN}
MAX_RECORDS=${MAX_RECORDS_IN}
EOF
chmod 600 "$SCRIPT_DIR/.env"
info "Written: $SCRIPT_DIR/.env"

# ── Step 6: Start nginx with HTTP-only config (needed for ACME challenge) ─────
info "Starting HTTP-only nginx for ACME challenge..."
cp "$SCRIPT_DIR/nginx/nginx-init.conf" "$SCRIPT_DIR/nginx/nginx.conf"
docker compose -f "$SCRIPT_DIR/docker-compose.yml" up -d nginx certbot
sleep 5  # give nginx a moment to bind port 80

# ── Step 7: Issue Let's Encrypt certificate ───────────────────────────────────
info "Issuing TLS certificate for $DOMAIN ..."
docker compose -f "$SCRIPT_DIR/docker-compose.yml" run --rm certbot \
    certonly \
    --webroot -w /var/www/certbot \
    --email "$LE_EMAIL" \
    --agree-tos --no-eff-email \
    -d "$DOMAIN"

# ── Step 8: Swap to HTTPS nginx config and start everything ───────────────────
info "Generating HTTPS nginx config..."
sed "s/DOMAIN_PLACEHOLDER/${DOMAIN}/g" \
    "$SCRIPT_DIR/nginx/nginx.conf.template" \
    > "$SCRIPT_DIR/nginx/nginx.conf"

info "Starting full stack (backend + nginx + certbot renewal daemon)..."
docker compose -f "$SCRIPT_DIR/docker-compose.yml" up -d
docker compose -f "$SCRIPT_DIR/docker-compose.yml" exec nginx nginx -s reload

# ── Step 9: Install systemd service for auto-start on reboot ─────────────────
COMPOSE_BIN="$(command -v docker) compose"
sudo tee /etc/systemd/system/pm-monitor.service > /dev/null <<UNIT
[Unit]
Description=ESP32 PM Monitor (Docker Compose)
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
WorkingDirectory=${SCRIPT_DIR}
ExecStart=${COMPOSE_BIN} -f ${SCRIPT_DIR}/docker-compose.yml up -d
ExecStop=${COMPOSE_BIN} -f ${SCRIPT_DIR}/docker-compose.yml down
TimeoutStartSec=300

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable pm-monitor
info "Systemd service enabled: pm-monitor (starts automatically on reboot)"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo "═════════════════════════════════════════════════════════════════════════════"
echo " PM Monitor is live at  https://${DOMAIN}"
echo ""
echo " Dashboard login:"
echo "   Username : ${DASH_USER_IN}"
echo "   Password : (what you entered above)"
echo ""
echo " ESP32 config.h — set these values, then re-flash:"
echo "   #define STANDALONE_MODE  0"
echo "   #define SERVER_URL       \"https://${DOMAIN}\""
echo "   #define API_KEY          \"${API_KEY_IN}\""
echo ""
echo " Useful commands:"
echo "   docker compose -f ${SCRIPT_DIR}/docker-compose.yml logs -f   # live logs"
echo "   docker compose -f ${SCRIPT_DIR}/docker-compose.yml ps        # status"
echo "   sudo systemctl restart pm-monitor                             # restart all"
echo "═════════════════════════════════════════════════════════════════════════════"
