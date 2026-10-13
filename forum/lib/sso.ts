import { api, type ForumConfig } from "@vlt/api-client";
export function base64url(bytes: Uint8Array) {
  return btoa(String.fromCharCode(...bytes))
    .replaceAll("+", "-")
    .replaceAll("/", "_")
    .replaceAll("=", "");
}
export async function beginSignIn(locale: string) {
  if (navigator.userAgent.includes("VLTONE/Forum")) {
    location.assign("vltone://forum/sign-in");
    return;
  }
  const cfg = await api.request<ForumConfig>("/v1/forum/config");
  const verifier = base64url(crypto.getRandomValues(new Uint8Array(32)));
  const state = base64url(crypto.getRandomValues(new Uint8Array(32)));
  const challenge = base64url(
    new Uint8Array(
      await crypto.subtle.digest("SHA-256", new TextEncoder().encode(verifier)),
    ),
  );
  sessionStorage.setItem(
    "vlt-forum-sso",
    JSON.stringify({
      verifier,
      state,
      returnTo: location.pathname + location.search,
      created: Date.now(),
    }),
  );
  const destination = new URL("/forum-login", cfg.public_origin);
  destination.search = new URLSearchParams({
    challenge,
    state,
    redirect_uri: cfg.forum_origin + "/auth/callback",
    lang: locale,
  }).toString();
  location.assign(destination.href);
}
