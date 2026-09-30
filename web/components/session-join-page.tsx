"use client";

import { useEffect, useRef, useState } from "react";

const labels = {
  ru: {
    title: "Присоединиться к сессии",
    subtitle: "Откройте приглашение в VLTONE, чтобы работать над проектом вместе.",
    open: "Открыть VLTONE",
    code: "Код приглашения",
    copy: "Копировать код",
    copied: "Код скопирован.",
    copyFailed: "Не удалось скопировать код. Выделите его в поле и скопируйте вручную.",
    fallback: "Если приложение не открылось, разрешите браузеру запустить VLTONE или вставьте код в окно «Присоединиться к сессии» в приложении.",
    invalid: "В ссылке нет действительного кода приглашения. Попросите ведущего сессии прислать новую ссылку.",
    loading: "Проверяем приглашение…",
    download: "Скачать VLTONE",
  },
  en: {
    title: "Join a session",
    subtitle: "Open this invitation in VLTONE to work on the project together.",
    open: "Open VLTONE",
    code: "Invitation code",
    copy: "Copy code",
    copied: "Code copied.",
    copyFailed: "Could not copy the code. Select it in the field and copy it manually.",
    fallback: "If the app did not open, allow your browser to launch VLTONE or paste the code into Join a session in the app.",
    invalid: "This link has no valid invitation code. Ask the session leader for a new link.",
    loading: "Checking invitation…",
    download: "Download VLTONE",
  },
};

export function SessionJoinPage({ locale }: { locale: string }) {
  const t = locale === "ru" ? labels.ru : labels.en;
  const [code, setCode] = useState<string>();
  const [notice, setNotice] = useState<"copied" | "copyFailed">();
  const codeInput = useRef<HTMLInputElement>(null);

  useEffect(() => {
    // Fragments stay in the browser. Never redeem or send the invitation to
    // the website's server; joining belongs to the authenticated desktop app.
    const readInvitation = () => {
      const fragment = window.location.hash.slice(1);
      setCode(/^[0-9]{6,32}$/.test(fragment) ? fragment : "");
      setNotice(undefined);
    };
    readInvitation();
    window.addEventListener("hashchange", readInvitation);
    return () => window.removeEventListener("hashchange", readInvitation);
  }, []);

  async function copyCode() {
    if (!code) return;
    try {
      await navigator.clipboard.writeText(code);
      setNotice("copied");
    } catch {
      codeInput.current?.focus();
      codeInput.current?.select();
      setNotice("copyFailed");
    }
  }

  return <main id="main-content" className="vlt-auth-wrap">
    <section className="vlt-card vlt-card-pad vlt-auth-card vlt-stack" aria-labelledby="join-title">
      <div>
        <h1 id="join-title" className="vlt-title">{t.title}</h1>
        <p className="vlt-subtitle">{t.subtitle}</p>
      </div>
      {code === undefined ? <p role="status">{t.loading}</p> : code ? <>
        <a className="vlt-button" href={`vlt://join/${code}`}>{t.open}</a>
        <p className="vlt-subtitle">{t.fallback}</p>
        <label className="vlt-label" htmlFor="join-code">{t.code}</label>
        <input id="join-code" ref={codeInput} className="vlt-input" value={code} readOnly spellCheck={false} autoComplete="off" />
        <button type="button" className="vlt-button vlt-button-secondary" onClick={copyCode}>{t.copy}</button>
        <p role="status" aria-live="polite">{notice ? t[notice] : ""}</p>
      </> : <p className="vlt-error" role="alert">{t.invalid}</p>}
      <a className="vlt-link" href={`/releases?lang=${locale === "ru" ? "ru" : "en"}`}>{t.download}</a>
    </section>
  </main>;
}
