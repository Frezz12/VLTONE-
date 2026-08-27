# VLT account platform — Ubuntu deployment

This directory documents the non-Docker, single-instance deployment used by
the account platform. It is intentionally separate from the DAW packaging
scripts: the backend, public web app and admin app are three system services.

## First server setup

1. Install PostgreSQL 18, Node.js 24, Go 1.26.7, Apache (`proxy`, `ssl`,
   `headers`) and Certbot. Create a local `vltstudio` database and a dedicated
   non-login `vltaccount` system user.
2. Copy the repository to `/opt/vlt-account-platform`, then create
   `/etc/vlt-account/api.env` owned by `root:vltaccount` with mode `0640`.
   Start from [`backend/.env.example`](../../backend/.env.example), set
   `APP_ENV=production`, HTTPS public/admin/API origins, a generated 32-byte
   `AUTH_SIGNING_SEED`, a positive `AI_GLOBAL_MONTHLY_TOKEN_LIMIT`, and real
   SMTP credentials. Never put this file in Git.
3. Install the three `vlt-account-*.service` files from this server's approved
   configuration. They must bind API/site/admin only to `127.0.0.1` ports
   `8080`, `3100`, and `3101` respectively.
4. Configure Apache so `/api/` maps to `http://127.0.0.1:8080/`, the public
   domain maps to port `3100`, and the admin domain maps to port `3101`.
   Pass `X-Forwarded-Proto: https` in the TLS vhosts. Obtain a certificate with
   `certbot --apache --redirect -d <public-domain> -d <admin-domain>`.
5. Run `sudo ./packaging/server/deploy.sh`, then create the initial owner:

   ```sh
   sudo -u vltaccount bash -c '
     set -a; source /etc/vlt-account/api.env; set +a
     exec /opt/vlt-account-platform/bin/vlt-adminctl create-owner \
       --email owner@example.com --nickname Owner
   '
   ```

The production config deliberately refuses to start without an SMTP host,
signing seed, HTTPS origins, and a global AI monthly limit. Set `AI_ENABLED`
to `false` until provider keys and spend controls are approved.

## Updating

Update the source checkout using the approved delivery method, then run:

```sh
sudo ./packaging/server/deploy.sh
```

The script builds Go binaries and both Next applications, applies only forward
SQL migrations, and restarts the three services. It never writes secrets or
database credentials into the checkout.
