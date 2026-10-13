"use client";
import {
  api,
  type AccountSession,
  type APIError,
  type ForumConfig,
} from "@vlt/api-client";
import { useEffect, useRef, useState } from "react";
import Link from "next/link";

export function ForumLogin({ locale }: { locale: string }) {
  const started = useRef(false);
  const [error, setError] = useState("");
  const ru = locale === "ru";
  useEffect(() => {
    if (started.current) return;
    started.current = true;
    async function authorize() {
      const query = new URLSearchParams(location.search);
      const challenge = query.get("challenge"),
        state = query.get("state"),
        redirect_uri = query.get("redirect_uri");
      const cfg = await api.request<ForumConfig>("/v1/forum/config");
      if (
        !challenge ||
        !state ||
        !/^[A-Za-z0-9_-]{43}$/.test(challenge) ||
        !/^[A-Za-z0-9_-]{43}$/.test(state) ||
        redirect_uri !== cfg.forum_origin + "/auth/callback"
      )
        throw new Error(
          ru
            ? "Ссылка входа недействительна. Начните вход на форуме заново."
            : "Invalid sign-in link. Start again on the forum.",
        );
      let session: AccountSession;
      try {
        session = await api.request<AccountSession>("/v1/me");
      } catch (e) {
        if (
          [
            "authentication_required",
            "session_expired",
            "account_unavailable",
          ].includes((e as APIError).code)
        ) {
          location.replace(
            `/login?lang=${locale}&returnTo=${encodeURIComponent(location.pathname + location.search)}`,
          );
          return;
        }
        throw e;
      }
      const result = await api.json<{
        code: string;
        state: string;
        redirect_uri: string;
      }>(
        "/v1/forum/auth/authorize",
        "POST",
        { challenge, state, redirect_uri },
        session.csrf_token,
      );
      const target = new URL(redirect_uri);
      target.hash = new URLSearchParams({
        code: result.code,
        state: result.state,
      }).toString();
      location.replace(target.href);
    }
    authorize().catch((e) =>
      setError(e.message || (ru ? "Не удалось войти." : "Sign in failed.")),
    );
  }, [locale, ru]);
  return (
    <main
      id="main-content"
      className="vlt-container"
      style={{ paddingBlock: 96 }}
    >
      <h1 className="vlt-title">VLTONE Forum</h1>
      {error ? (
        <>
          <p className="vlt-error" role="alert">
            {error}
          </p>
          <Link href="/account">{ru ? "Мой аккаунт" : "My account"}</Link>
        </>
      ) : (
        <p role="status">
          {ru
            ? "Входим с вашим аккаунтом VLTONE…"
            : "Signing in with your VLTONE account…"}
        </p>
      )}
    </main>
  );
}
