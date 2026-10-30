# VPS deployment record

Recorded 2026-10-29 (Asia/Shanghai).

## Verified state

- Host: `43.108.80.211`, Ubuntu 24.04.2 LTS, Linux x86_64.
- Installed binary: `/usr/local/bin/dropspace-server`.
- Configuration: `/etc/dropspace/config.json`.
- Data: `/var/lib/dropspace`, owned by the dedicated `dropspace` system account with mode `0700`.
- Unit: `/etc/systemd/system/dropspace.service`, enabled and active.
- Application listener: `127.0.0.1:8787` only.
- Public endpoint: `https://43.108.80.211:8443`, terminated by Caddy 2.6.2.
- Local and public health checks: `{"status":"ok"}`. The public Windows check returned HTTP 200 with normal certificate verification; no insecure override was used.
- The current manifest-capable Linux binary SHA-256 is `9cce7633ef079b6907b0b7c80fc495fa96e3dcd474766d9ead805ec2f72455ac`; systemd returned `active` after its 2026-10-29 upgrade.

The temporary upload copies in `/home/lyu` were removed after installation. No SSH password or application token is stored in this repository.

## Public HTTPS and certificate renewal

TCP 443 remains owned by the existing `xray.service`; it was not modified. Caddy listens only on TCP/UDP 8443 and proxies to the loopback application. TCP 80 remains available for ACME validation during renewal and does not serve the DropSpace API.

Let’s Encrypt issued a short-lived certificate whose critical SAN is `IP Address:43.108.80.211`; the observed validity window was 2026-10-29 through 2026-11-05. Certbot 5.8.0 is installed from its official stable snap. `snap.certbot.renew.timer` is enabled, and `/etc/letsencrypt/renewal-hooks/deploy/dropspace-caddy` copies renewed material into the Caddy-readable directory and reloads Caddy. A full `certbot renew --dry-run --run-deploy-hooks` simulation succeeded. The temporary staging certificate was deleted after validation.

Active services were rechecked after configuration: `xray`, `dropspace`, and `caddy` were all active. Clients should use exactly `https://43.108.80.211:8443`; they must not disable certificate validation.

The Windows Qt integration client subsequently paired through this endpoint and uploaded both a raw regular-file object and a folder ZIP object. The server published both as ready, and the client committed the returned two-entry snapshot to its local SQLite database. This exercised more than the unauthenticated health route while keeping application tokens out of the repository.

The host also reported pending OS updates and a required restart. No upgrade or reboot was performed because that is separate from the DropSpace deployment and could affect existing services.

