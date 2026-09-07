# Persistent desktop sign-in

The 72-hour entitlement is an offline allowance, not a password expiration.
Online refresh remains automatic (at startup and every 12 minutes for the
15-minute access token). Refresh credentials last 30 days and are renewed on
successful rotation. Ordinary restarts do not require the account password.

Before requesting a rotation, the desktop saves a random UUID alongside the old
refresh credential in the OS vault and verifies it can read that envelope back.
It sends the UUID as `Idempotency-Key`. The same ID survives network failures,
crashes and failed saves of a rotated credential. Successful persistence of the
new credential removes the ID. A per-user process lock serializes vault reads,
rotation and writes across multiple running copies. Logout cancels pending
restore work before clearing the vault.

Migration **000023_desktop_refresh_retry** adds a hashed operation ID and the
successor session ID to the consumed session. The server stores these in the
rotation transaction. Tokens can be reproduced with domain-separated HMACs
using the server signing secret; plaintext refresh credentials are not stored
in PostgreSQL. A retry must provide the same old credential and operation ID,
and the successor must still belong to the same active user/device and must not
be expired, revoked or rotated. A retry cannot extend the refresh lifetime or
revive an old session. A different or missing ID retains the existing token
reuse rejection and account-wide revocation behavior.

The HTTP header keeps new desktop requests compatible with older servers;
older servers ignore it and keep their old rotation behavior. Deploy migration
23 and the updated API to enable recovery from a committed but lost response.
Existing desktop clients can also continue using the updated server. Updating
the desktop is required for durable request IDs and vault recovery controls.

Temporary failures, malformed success responses, and proxy 401/403 responses
retain the saved credential and use a still-valid signed offline entitlement.
Only explicit account API rejection codes clear the saved session. Database
failures return a temporary error rather than a credential rejection. After
the offline allowance expires, an online check is required; the saved refresh
credential is retained so that reconnecting does not require a password.

A locked or inaccessible OS vault is distinguished from an absent session.
Automatic restore remains noninteractive. **Восстановить сохранённый вход** on
the startup form explicitly allows the OS to authorize access to the existing
credential. This can be necessary after replacing an ad-hoc signed macOS build;
the account password is not needed if the saved server credential remains valid.
The implementation does not weaken Keychain ACLs or store secrets in QSettings.
Apple describes how Keychain tracks application identities in
[macOS Code Signing In Depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html).

Verification: `account_restore_test` uses the real service with an isolated HTTP
server and fake vault; `secure_storage_mac_test` exercises the real vault code
against a fake Security API. `TestPostgresAccountFlow/refresh_retry_after_restart`
tests legacy clients, a committed rotation followed by response failure,
concurrent duplicate requests, stale receipts and reuse revocation on PostgreSQL.
