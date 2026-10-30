# VPS deployment record

Recorded 2026-10-29 and updated 2026-10-30 (Asia/Shanghai).

## Verified state

- Host: `43.108.80.211`, Ubuntu 24.04.2 LTS, Linux x86_64.
- Installed binary: `/usr/local/bin/dropspace-server`.
- Configuration: `/etc/dropspace/config.json`.
- Data: `/var/lib/dropspace`, owned by the dedicated `dropspace` system account with mode `0700`.
- Unit: `/etc/systemd/system/dropspace.service`, enabled and active.
- Application listener: `127.0.0.1:8787` only.
- Public endpoint: `https://43.108.80.211:8443`, terminated by Caddy 2.6.2.
- Local and public health checks: `{"status":"ok"}`. The public Windows check returned HTTP 200 with normal certificate verification; no insecure override was used.
- The current phase-two Linux binary is 11,415,712 bytes with SHA-256 `b6d68ccb676c8d5180a9b05dedfbb8e31d5e549a84b1c651076203a6c1e9b847`; systemd returned `active` after its 2026-10-30 upgrade.

The temporary upload copies in `/home/lyu` were removed after installation. No SSH password or application token is stored in this repository.

## Public HTTPS and certificate renewal

TCP 443 remains owned by the existing `xray.service`; it was not modified. Caddy listens only on TCP/UDP 8443 and proxies to the loopback application. TCP 80 remains available for ACME validation during renewal and does not serve the DropSpace API.

Let’s Encrypt issued a short-lived certificate whose critical SAN is `IP Address:43.108.80.211`; the observed validity window was 2026-10-29 through 2026-11-05. Certbot 5.8.0 is installed from its official stable snap. `snap.certbot.renew.timer` is enabled, and `/etc/letsencrypt/renewal-hooks/deploy/dropspace-caddy` copies renewed material into the Caddy-readable directory and reloads Caddy. A full `certbot renew --dry-run --run-deploy-hooks` simulation succeeded. The temporary staging certificate was deleted after validation.

Active services were rechecked after configuration: `xray`, `dropspace`, and `caddy` were all active. Clients should use exactly `https://43.108.80.211:8443`; they must not disable certificate validation.

The Windows Qt integration client subsequently paired through this endpoint and uploaded both a raw regular-file object and a folder ZIP object. The server published both as ready, and the client committed the returned two-entry snapshot to its local SQLite database. This exercised more than the unauthenticated health route while keeping application tokens out of the repository.

The host also reported pending OS updates and a required restart. No upgrade or reboot was performed because that is separate from the DropSpace deployment and could affect existing services.

## Phase-two deployment and rollback

Before replacement, an online SQLite backup plus the old binary, application configuration, and systemd unit were saved under `/var/backups/dropspace/20261030T103734Z`. The backup database returned `PRAGMA quick_check=ok`. The candidate was hash-verified, installed atomically as `/usr/local/bin/dropspace-server`, and `dropspace.service` was restarted once at 2026-10-30 18:51 CST.

Startup applied additive schema migration version 2. Production then reported `migration_v2=1`, both clipboard tables present, and `PRAGMA quick_check=ok`. Loopback and trusted public health checks passed. Public authenticated tests passed the original file upload/snapshot/download path plus clipboard text, PNG, thumbnail, HTTP history/latest/content/delete/clear, and WSS snapshot/create notification. Test clipboard blobs were cleared; the clipboard object directories contained no residual test files. `dropspace`, `caddy`, and `xray` remained active, and the DropSpace journal contained no warning-or-higher entries after deployment.

Rollback, if required, is:

1. stop `dropspace.service`;
2. restore `dropspace-server` and `dropspace.sqlite` from the timestamped backup;
3. preserve the failed database separately for diagnosis;
4. start the service and verify loopback/public health plus an authenticated file snapshot.

Do not drop clipboard tables in place. Caddy, certificates, xray, firewall rules, and `/etc/dropspace/config.json` were not modified by this phase-two deployment.

