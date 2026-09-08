# Persistent desktop sign-in

The 72-hour entitlement is an offline allowance, not a password expiration.
Online refresh remains automatic (at startup and every 12 minutes for the
15-minute access token). Refresh credentials last 30 days and are renewed on
successful rotation. Ordinary restarts do not require the account password.

Before requesting a rotation, the desktop saves a random UUID alongside the old
refresh credential in the credential store and verifies it can read that envelope back.
It sends the UUID as `Idempotency-Key`. The same ID survives network failures,
crashes and failed saves of a rotated credential. Successful persistence of the
new credential removes the ID. A per-user process lock serializes credential reads,
rotation and writes across multiple running copies. Logout cancels pending
restore work before clearing the credential store. A failed local deletion reports an
error so sign-out can be retried.

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
the desktop is required for durable request IDs and credential recovery controls.

Temporary failures, malformed success responses, and proxy 401/403 responses
retain the saved credential and use a still-valid signed offline entitlement.
Only explicit account API rejection codes clear the saved session. Database
failures return a temporary error rather than a credential rejection. After
the offline allowance expires, an online check is required; the saved refresh
credential is retained so that reconnecting does not require a password.

On macOS, the account credential envelope is now stored as unencrypted JSON at
`QStandardPaths::AppLocalDataLocation/credentials/desktop-session.json` (normally
`~/Library/Application Support/VLT Studio/VLT Studio Pro/credentials/desktop-session.json`).
The `credentials` directory has mode `0700`; the file has mode `0600`. This keeps
other OS users out, but programs running as the same user can read the token.
The file contains the refresh token, reporter token and signed offline metadata;
the account password and short-lived access token are not persisted.

Writes use a private temporary file, sync its contents, then atomically rename it
and sync the directory. Failed writes before the rename preserve the previous
session. Reads reject symlinks, non-regular files, foreign owners and hard links;
the credential directory is opened without following a symlink. Existing
permissions are tightened when accessing storage. Logout removes the local file.

Account operations on macOS never access the old Keychain account record, even
for an explicit restore or sign-out. There is deliberately no migration: the
first launch after this change requires one account sign-in. The old record is
left untouched and is never used as a fallback, including after logout. Named
AI API keys retain their Keychain storage; Windows account sessions still use
Credential Manager. An inaccessible local store is reported separately from a
missing session, without asking for an operating-system password.

Verification: `account_restore_test` uses the real service with an isolated HTTP
server and fake vault; `secure_storage_mac_test` exercises the real vault code
against a fake Security API and uses a temporary local directory to verify
permissions, restart persistence, replacement failures, symlink handling and
zero Keychain calls for macOS account sessions. `TestPostgresAccountFlow/refresh_retry_after_restart`
tests legacy clients, a committed rotation followed by response failure,
concurrent duplicate requests, stale receipts and reuse revocation on PostgreSQL.
