#!/bin/sh
set -eu

install -d -o root -g caddy -m 0750 /etc/caddy/dropspace-cert
install -o root -g caddy -m 0640 /etc/letsencrypt/live/dropspace-ip/fullchain.pem /etc/caddy/dropspace-cert/fullchain.pem
install -o root -g caddy -m 0640 /etc/letsencrypt/live/dropspace-ip/privkey.pem /etc/caddy/dropspace-cert/privkey.pem

if systemctl is-active --quiet caddy.service; then
  systemctl reload caddy.service
fi

