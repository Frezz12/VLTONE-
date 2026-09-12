import Link from "next/link";
import { BrandMark } from "./brand-mark";

export function Footer({ locale }: { locale: string }) {
  const ru = locale === "ru";
  return <footer className="site-footer"><div className="footer-inner">
    <Link className="footer-brand" href={`/${locale}`}><BrandMark /><span>VLTone</span></Link>
    <p>{ru ? "Пространство для вашей музыки." : "A space for your music."}</p>
    <nav aria-label={ru ? "Ссылки в подвале" : "Footer navigation"}>
      <Link href={`/${locale}/register`}>{ru ? "Открытая бета" : "Open beta"}</Link>
      <Link href={`/${locale}/releases`}>{ru ? "Скачать" : "Download"}</Link>
      <Link href={`/${locale}/manual`}>{ru ? "Руководство" : "Guide"}</Link>
      <Link href={`/${locale}/bug-report`}>{ru ? "Сообщить о баге" : "Report a bug"}</Link>
      <Link href={`/${locale}/account`}>{ru ? "Личный кабинет" : "Your account"}</Link>
    </nav>
    <div className="footer-bottom"><span>© {new Date().getFullYear()} VLTone</span><span>{ru ? "Программа для создания музыки · Windows и macOS" : "Music creation software · Windows and macOS"}</span><span>vltstudio.ru</span></div>
  </div></footer>;
}
