"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { BrandMark } from "./brand-mark";

export function Header({ locale }: { locale: string }) {
  const ru = locale === "ru";
  const other = ru ? "en" : "ru";
  const pathname = usePathname();

  function switchLocale() {
    document.cookie = `vlt-locale=${other}; max-age=31536000; path=/; samesite=lax`;
    window.location.reload();
  }

  return <>
    <a className="skip-link" href="#main-content">{ru ? "Перейти к содержимому" : "Skip to content"}</a>
    <header className="vlt-topbar">
      <Link className="vlt-brand" href="/" aria-label="VLTone"><BrandMark /><span>VLTone</span></Link>
      <nav className="vlt-nav" aria-label={ru ? "Навигация сайта" : "Site navigation"}>
        <Link href="/#overview">{ru ? "О программе" : "Overview"}</Link>
        <a href="/releases" aria-current={pathname.includes("/releases") ? "page" : undefined}>{ru ? "Скачать" : "Download"}</a>
        <Link href="/manual" aria-current={pathname.includes("/manual") ? "page" : undefined}>{ru ? "Инструкция" : "Manual"}</Link>
        <Link href="/account" aria-current={pathname.includes("/account") ? "page" : undefined}>{ru ? "Аккаунт" : "Account"}</Link>
      </nav>
      <button className="locale-link" type="button" onClick={switchLocale} aria-label={ru ? "Open in English" : "Открыть на русском"}>{other.toUpperCase()}</button>
    </header>
  </>;
}
