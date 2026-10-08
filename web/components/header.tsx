"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { BrandMark } from "./brand-mark";
import { ArrowUpRight, CircleUserRound, Globe2 } from "lucide-react";
import { readCookiePreference } from "@/lib/cookie-preference";

export function LocaleSwitch({ locale, className = "" }: { locale: string; className?: string }) {
  const ru = locale === "ru";
  const other = ru ? "en" : "ru";

  function switchLocale() {
    if (readCookiePreference() !== "necessary") {
      const target = new URL(window.location.href);
      target.searchParams.set("lang", other);
      window.location.assign(target);
      return;
    }
    document.cookie = `vlt-locale=${other}; max-age=31536000; path=/; samesite=lax`;
    const target = new URL(window.location.href);
    target.searchParams.delete("lang");
    window.location.assign(target);
  }
  return <button className={`header-icon locale-link ${className}`} type="button" onClick={switchLocale} aria-label={ru ? "Open in English" : "Открыть на русском"} title={ru ? "English" : "Русский"}><Globe2 size={20} strokeWidth={1.8} aria-hidden />{className && <span>{ru ? "EN" : "RU"}</span>}</button>;
}

export function Header({ locale }: { locale: string }) {
  const ru = locale === "ru";
  const pathname = usePathname();

  return <>
    <a className="skip-link" href="#main-content">{ru ? "Перейти к содержимому" : "Skip to content"}</a>
    <header className="vlt-topbar">
      <div className="header-identity">
        <Link className="vlt-brand" href="/" aria-label="VLTone"><BrandMark /><span className="brand-wordmark">VLT<span>one</span></span></Link>
        <nav className="vlt-nav" aria-label={ru ? "Навигация сайта" : "Site navigation"}>
          <Link href="/capabilities" aria-current={pathname.includes("/capabilities") ? "page" : undefined}>{ru ? "Возможности" : "Capabilities"}</Link>
          <a href="/releases" aria-current={pathname.includes("/releases") ? "page" : undefined}>{ru ? "Скачать" : "Download"}</a>
          <Link href="/manual" aria-current={pathname.includes("/manual") ? "page" : undefined}>{ru ? "Инструкция" : "Manual"}</Link>
        </nav>
      </div>
      <div className="header-navigation">
        <Link className="header-start" href="/register">{ru ? "Создать аккаунт" : "Create account"}<ArrowUpRight size={15} aria-hidden /></Link>
        <LocaleSwitch locale={locale} />
        <Link className="header-icon account-link" href="/account" aria-label={ru ? "Аккаунт" : "Account"} title={ru ? "Аккаунт" : "Account"} aria-current={pathname.includes("/account") ? "page" : undefined}><CircleUserRound size={21} strokeWidth={1.8} aria-hidden /></Link>
      </div>
    </header>
  </>;
}
