import Link from "next/link";
import { BrandMark } from "./brand-mark";
import { CookieSettingsButton } from "./cookie-notice";

export function Footer({ locale }: { locale: string }) {
  const ru = locale === "ru";
  return <footer className="site-footer"><div className="footer-inner">
    <Link className="footer-brand" href="/"><BrandMark /><span>VLTone</span></Link>
    <p>{ru ? "Место, где идеи становятся музыкой." : "Where ideas become music."}<br />Windows / macOS</p>
    <nav aria-label={ru ? "Ссылки в подвале" : "Footer navigation"}>
      <Link href="/register">{ru ? "Открытая бета" : "Open beta"}</Link>
      <Link href="/capabilities">{ru ? "Возможности" : "Capabilities"}</Link>
      <a href="/releases">{ru ? "Скачать" : "Download"}</a>
      <Link href="/manual">{ru ? "Руководство" : "Guide"}</Link>
      <Link href="/bug-report">{ru ? "Сообщить о баге" : "Report a bug"}</Link>
      <Link href="/account">{ru ? "Личный кабинет" : "Your account"}</Link>
    </nav>
    <div className="footer-legal"><Link href="/terms">{ru ? "Пользовательское соглашение" : "Terms of use"}</Link><Link href="/privacy">{ru ? "Персональные данные" : "Privacy policy"}</Link><Link href="/consent">{ru ? "Согласие на обработку данных" : "Data processing consent"}</Link><CookieSettingsButton locale={locale} /><a href="mailto:vltmscw@outlook.com">vltmscw@outlook.com</a></div>
    <div className="footer-bottom"><span>© {new Date().getFullYear()} VLTone</span><span>{ru ? "Запись аудио, MIDI и сведение." : "Audio recording, MIDI and mixing."}</span><span>vltstudio.ru</span></div>
  </div></footer>;
}
