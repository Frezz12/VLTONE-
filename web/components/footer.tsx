import Link from "next/link";
import { BrandMark } from "./brand-mark";

export function Footer({ locale }: { locale: string }) {
  const ru = locale === "ru";
  return <footer className="site-footer"><div className="footer-inner">
    <Link className="footer-brand" href="/"><BrandMark /><span>VLTone</span></Link>
    <p>{ru ? "DAW для Windows и macOS." : "A DAW for Windows and macOS."}</p>
    <nav aria-label={ru ? "Ссылки в подвале" : "Footer navigation"}>
      <Link href="/register">{ru ? "Открытая бета" : "Open beta"}</Link>
      <Link href="/capabilities">{ru ? "Возможности" : "Capabilities"}</Link>
      <a href="/releases">{ru ? "Скачать" : "Download"}</a>
      <Link href="/manual">{ru ? "Руководство" : "Guide"}</Link>
      <Link href="/bug-report">{ru ? "Сообщить о баге" : "Report a bug"}</Link>
      <Link href="/account">{ru ? "Личный кабинет" : "Your account"}</Link>
    </nav>
    <div className="footer-bottom"><span>© {new Date().getFullYear()} VLTone</span><span>{ru ? "Программа для создания музыки · Windows и macOS" : "Music creation software · Windows and macOS"}</span><span>vltstudio.ru</span></div>
  </div></footer>;
}
