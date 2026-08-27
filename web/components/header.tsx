import Link from "next/link";
import { Bug, CircleUserRound } from "lucide-react";

export function Header({ locale }: { locale: string }) {
  const other = locale === "ru" ? "en" : "ru";
  return (
    <header className="vlt-topbar">
      <Link className="vlt-brand" href={`/${locale}`}><span className="vlt-brand-mark">VLT</span><span>Studio Pro</span></Link>
      <nav className="vlt-nav" aria-label={locale === "ru" ? "Навигация аккаунта" : "Account navigation"}>
        <Link className="vlt-nav-optional" href={`/${locale}/bug-report`}><Bug size={16} aria-hidden /> {locale === "ru" ? "Сообщить о баге" : "Report a bug"}</Link>
        <Link href={`/${locale}/account`}><CircleUserRound size={16} aria-hidden /> {locale === "ru" ? "Аккаунт" : "Account"}</Link>
        <Link href={`/${other}`} hrefLang={other}>{other.toUpperCase()}</Link>
      </nav>
    </header>
  );
}
