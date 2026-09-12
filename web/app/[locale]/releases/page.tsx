import Link from "next/link";
import { ArrowRight, CalendarDays, PackageOpen } from "lucide-react";
import { getReleases } from "@/lib/releases";
import { siteMetadata } from "@/lib/seo";

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  return siteMetadata(locale, "/releases", locale === "ru" ? "Скачать VLTone — открытая бета и обновления" : "Download VLTone — open beta and releases", locale === "ru" ? "Скачайте VLTone для своей системы. Актуальные установщики, новые возможности и исправления музыкальной программы. После установки войдите в аккаунт." : "Download VLTone for your system. Available installers, new features, and fixes for the music creation app. Sign in to your account after installation.");
}

export const dynamic = "force-dynamic";

export default async function ReleasesPage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const releases = await getReleases(locale);
  const ru = locale === "ru";
  return <main id="main-content" className="releases-main">
    <header className="releases-hero"><span className="pill-tag">{ru ? "VLTone · Открытая бета" : "VLTone · Open beta"}</span><h1>{ru ? "Обновления" : "Releases"}</h1><p>{ru ? "Выберите версию и скачайте установщик для своей системы. Установите VLTone, затем войдите в программе с почтой и паролем от аккаунта на сайте." : "Choose a version and download the installer for your system. Install VLTone, then sign in to the app with your website account email and password."}</p><Link className="text-link" href={`/${locale}/register`}>{ru ? "Нет аккаунта? Зарегистрироваться" : "Need an account? Register"}<ArrowRight size={14} aria-hidden /></Link></header>
    {releases.length === 0 ? <section className="releases-empty"><PackageOpen size={28} aria-hidden /><h2>{ru ? "Релизов пока нет" : "No releases yet"}</h2><p>{ru ? "Первая опубликованная версия появится здесь." : "The first published version will appear here."}</p></section> : <div className="release-timeline">{releases.map((release, index) => <article className="release-summary-card" key={release.id}><span className="release-index">{String(index + 1).padStart(2, "0")}</span><div><div className="release-summary-meta"><strong>v{release.version}</strong><span><CalendarDays size={14} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "long" }).format(new Date(release.published_at))}</span></div><h2>{release.summary}</h2><div className="release-platforms">{[...new Set(release.artifacts.map((item) => item.platform))].map((platform) => <span className="vlt-badge" key={platform}>{platform === "macos" ? "macOS" : platform === "windows" ? "Windows" : "Linux"}</span>)}</div></div><Link className="release-open" href={`/${locale}/releases/${release.version}`} aria-label={`${ru ? "Открыть версию" : "Open version"} ${release.version}`}><ArrowRight size={20} aria-hidden /></Link></article>)}</div>}
  </main>;
}
