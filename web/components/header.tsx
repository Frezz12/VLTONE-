"use client";

import Link from "next/link";
import { usePathname } from "next/navigation";
import { BrandMark } from "./brand-mark";

export function Header({ locale }: { locale: string }) {
  const ru = locale === "ru";
  const other = ru ? "en" : "ru";
  const pathname = usePathname();
  const parts = pathname.split("/");
  parts[1] = other;
  const localeHref = parts.join("/") || `/${other}`;
  return <>
    <a className="skip-link" href="#main-content">{ru ? "Перейти к содержимому" : "Skip to content"}</a>
    <header className="vlt-topbar">
      <Link className="vlt-brand" href={`/${locale}`} aria-label="VLTone"><BrandMark /><span>VLTone</span></Link>
      <nav className="vlt-nav" aria-label={ru ? "Навигация сайта" : "Site navigation"}>
        <Link href={`/${locale}#overview`}>{ru ? "О программе" : "Overview"}</Link>
        <Link href={`/${locale}/releases`} aria-current={pathname.includes("/releases") ? "page" : undefined}>{ru ? "Скачать" : "Download"}</Link>
        <Link href={`/${locale}/manual`} aria-current={pathname.includes("/manual") ? "page" : undefined}>{ru ? "Инструкция" : "Manual"}</Link>
        <Link href={`/${locale}/account`} aria-current={pathname.includes("/account") ? "page" : undefined}>{ru ? "Аккаунт" : "Account"}</Link>
      </nav>
      <Link className="locale-link" href={localeHref} hrefLang={other} lang={other} onClick={(event) => {
        // Preserve the current manual chapter when switching language.
        if (window.location.hash) { event.preventDefault(); window.location.assign(localeHref + window.location.hash); }
      }} aria-label={ru ? "Open in English" : "Открыть на русском"}>{other.toUpperCase()}</Link>
    </header>
  </>;
}
