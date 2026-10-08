# Registration CAPTCHA

The website reads the public configuration from `/v1/meta`. The Go API verifies
`captcha_token` with Cloudflare Siteverify before hashing the password or creating
an account. Every registration attempt requires a fresh token. Successful tokens
must have the `register` action and match the hostname in `PUBLIC_ORIGIN`.

## Production setup

1. Create a Managed Turnstile widget in your Cloudflare account. Allow the public
   website hostname (for example `vltstudio.ru`), matching `PUBLIC_ORIGIN`.
2. Set `TURNSTILE_SITE_KEY` and `TURNSTILE_SECRET_KEY` in the backend environment.
   The secret belongs only on the server; do not put it in `NEXT_PUBLIC_*`, the
   repository, browser storage, or frontend build settings.
3. Deploy the updated backend and website. The frontend receives the public key
   at runtime, so key rotation does not need a frontend rebuild.
4. Check registration in a normal browser and verify that missing, expired, and
   reused tokens are rejected by the API.

Production registration requires CAPTCHA even when keys are missing. Missing
configuration or a provider outage returns `503 captcha_unavailable`; invalid
tokens return `422 captcha_invalid`. Existing-account login remains available.
There is no production bypass and no fallback that accepts an unchecked token.

## Local testing

Local development without keys keeps registration available without CAPTCHA.
Supplying either key enables CAPTCHA validation; both are needed to register.
The official visible pass site key `1x00000000000000000000AA` and pass secret
`1x0000000000000000000000000000000AA` can be used locally. Test keys are rejected
when `APP_ENV=production`.

Browser tests stub only the external widget script. Backend tests exercise
validation failures, hostname/action checks, network errors, metadata secrecy,
and account creation through the PostgreSQL integration flow.

References: [widget setup](https://developers.cloudflare.com/turnstile/get-started/client-side-rendering/),
[Siteverify](https://developers.cloudflare.com/turnstile/get-started/server-side-validation/),
[test keys](https://developers.cloudflare.com/turnstile/troubleshooting/testing/).
