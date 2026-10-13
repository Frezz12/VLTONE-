"use client";
import { useEffect, useRef, useState } from "react";
import { api } from "@vlt/api-client";
import { useForum } from "@/components/shell";
export default function Callback() {
  const once = useRef(false);
  const [error, setError] = useState("");
  const { t } = useForum();
  useEffect(() => {
    if (once.current) return;
    once.current = true;
    const params = new URLSearchParams(location.hash.slice(1));
    history.replaceState(null, "", location.pathname);
    const raw = sessionStorage.getItem("vlt-forum-sso");
    sessionStorage.removeItem("vlt-forum-sso");
    void (async () => {
      try {
        const saved = JSON.parse(raw ?? "null");
        if (
          !saved ||
          saved.state !== params.get("state") ||
          Date.now() - saved.created > 10 * 60 * 1000
        )
          throw Error(
            t("Вход устарел. Попробуйте снова.", "Sign-in expired. Try again."),
          );
        await api.json("/v1/forum/auth/exchange", "POST", {
          code: params.get("code"),
          state: saved.state,
          verifier: saved.verifier,
        });
        location.replace(
          typeof saved.returnTo === "string" &&
            saved.returnTo.startsWith("/") &&
            !saved.returnTo.startsWith("//")
            ? saved.returnTo
            : "/",
        );
      } catch (e) {
        setError((e as Error).message);
      }
    })();
  }, [t]);
  return (
    <div className="empty">
      <h1>{t("Вход в сообщество", "Signing in")}</h1>
      <p role={error ? "alert" : "status"}>
        {error ||
          t("Возвращаем вас на форум…", "Taking you back to the forum…")}
      </p>
      <a href="/">{t("На главную", "Go to feed")}</a>
    </div>
  );
}
