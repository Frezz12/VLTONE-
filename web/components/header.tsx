"use client";

import Link from "next/link";
import { BookOpenText, Bug, CircleUserRound } from "lucide-react";
import { usePathname } from "next/navigation";

export function Header({ locale }: { locale: string }) {
  const other = locale === "ru" ? "en" : "ru";
  const pathname = usePathname();
  const parts = pathname.split("/");
  parts[1] = other;
  const localeHref = parts.join("/") || `/${other}`;
  return (
    <header className="vlt-topbar">
      <Link className="vlt-brand" href={`/${locale}`}><span className="vlt-brand-mark">VLT</span><span>Studio Pro</span></Link>
      <nav className="vlt-nav" aria-label={locale === "ru" ? "Навигация сайта" : "Site navigation"}>
        <Link href={`/${locale}/manual`}><BookOpenText size={16} aria-hidden /> {locale === "ru" ? "Инструкция" : "Manual"}</Link>
        <Link className="vlt-nav-optional" href={`/${locale}/bug-report`}><Bug size={16} aria-hidden /> {locale === "ru" ? "Сообщить о баге" : "Report a bug"}</Link>
        <Link href={`/${locale}/account`}><CircleUserRound size={16} aria-hidden /> {locale === "ru" ? "Аккаунт" : "Account"}</Link>
        <Link href={localeHref} hrefLang={other} aria-label={locale === "ru" ? "Open in English" : "Открыть на русском"}>{other.toUpperCase()}</Link>
      </nav>
    </header>
  );
}
