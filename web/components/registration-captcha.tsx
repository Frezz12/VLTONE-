"use client";

import { useEffect, useRef, useState } from "react";
import { useTranslations } from "next-intl";
import { RefreshCw } from "lucide-react";

type Turnstile = { render: (target: HTMLElement, options: Record<string, unknown>) => string; remove: (id: string) => void };
declare global { interface Window { turnstile?: Turnstile } }
let loader: Promise<Turnstile> | undefined;

function loadTurnstile() {
  if (window.turnstile) return Promise.resolve(window.turnstile);
  if (loader) return loader;
  loader = new Promise<Turnstile>((resolve, reject) => {
    const script = document.createElement("script");
    script.src = "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit";
    script.async = true;
    const failed = () => { clearTimeout(timer); script.remove(); loader = undefined; reject(new Error("Turnstile unavailable")); };
    const timer = window.setTimeout(failed, 15000);
    script.onerror = failed;
    script.onload = () => { clearTimeout(timer); if (window.turnstile) resolve(window.turnstile); else failed(); };
    document.head.appendChild(script);
  });
  return loader;
}

export function RegistrationCaptcha({ siteKey, locale, reset, onToken }: { siteKey: string; locale: string; reset: number; onToken: (token: string) => void }) {
  const t = useTranslations("Auth");
  const container = useRef<HTMLDivElement>(null);
  const tokenCallback = useRef(onToken);
  tokenCallback.current = onToken;
  const [attempt, setAttempt] = useState(0);
  const [compact, setCompact] = useState(false);
  const [phase, setPhase] = useState<"loading" | "verified" | "error" | "expired">("loading");
  useEffect(() => {
    const element = container.current;
    if (!element) return;
    const measure = () => setCompact(element.clientWidth < 300);
    measure();
    const observer = new ResizeObserver(measure); observer.observe(element);
    return () => observer.disconnect();
  }, []);
  useEffect(() => {
    let alive = true;
    let widget: string | undefined;
    let api: Turnstile | undefined;
    tokenCallback.current(""); setPhase("loading");
    loadTurnstile().then(loaded => {
      api = loaded;
      if (!alive || !container.current) return;
      try {
        widget = loaded.render(container.current, {
            sitekey: siteKey, action: "register", theme: "dark", size: compact ? "compact" : "flexible", language: locale,
            "response-field": false,
            callback: (token: string) => { if (alive) { tokenCallback.current(token); setPhase("verified"); } },
            "expired-callback": () => { if (alive) { tokenCallback.current(""); setPhase("expired"); } },
            "error-callback": () => { if (alive) { tokenCallback.current(""); setPhase("error"); } },
            "timeout-callback": () => { if (alive) { tokenCallback.current(""); setPhase("expired"); } },
            "unsupported-callback": () => { if (alive) { tokenCallback.current(""); setPhase("error"); } },
        });
      } catch { if (alive) setPhase("error"); }
    }).catch(() => { if (alive) setPhase("error"); });
    return () => { alive = false; if (widget !== undefined) api?.remove(widget); };
  }, [siteKey, locale, reset, attempt, compact]);

  return <div className="auth-captcha" aria-label={t("captchaLabel")}>
    <span className="auth-captcha-label">{t("captchaLabel")}</span>
    <div ref={container} className="auth-captcha-widget" data-compact={compact} />
    <div className="auth-captcha-status"><span role="status">{t(phase === "verified" ? "captchaVerified" : phase === "expired" ? "captchaExpired" : phase === "error" ? "captchaError" : "captchaLoading")}</span>{(phase === "error" || phase === "expired") && <button type="button" onClick={() => setAttempt(value => value + 1)}><RefreshCw size={15} aria-hidden />{t("captchaRetry")}</button>}</div>
  </div>;
}
