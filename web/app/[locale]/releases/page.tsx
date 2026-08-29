import Link from "next/link";
import { ArrowRight, CalendarDays, PackageOpen } from "lucide-react";
import { getReleases } from "@/lib/releases";

export const dynamic = "force-dynamic";

export default async function ReleasesPage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const releases = await getReleases(locale);
  const ru = locale === "ru";
  return <main className="releases-main">
    <header className="releases-hero"><span className="release-eyebrow"><PackageOpen size={15} aria-hidden />VLT Studio Pro</span><h1>{ru ? "Обновления" : "Releases"}</h1><p>{ru ? "Новые возможности, изменения и исправления — вместе с доступными установщиками для каждой системы." : "New features, changes, fixes, and the installers currently available for each platform."}</p></header>
    {releases.length === 0 ? <section className="releases-empty"><PackageOpen size={28} aria-hidden /><h2>{ru ? "Релизов пока нет" : "No releases yet"}</h2><p>{ru ? "Первая опубликованная версия появится здесь." : "The first published version will appear here."}</p></section> : <div className="release-timeline">{releases.map((release, index) => <article className="release-summary-card" key={release.id}><span className="release-index">{String(index + 1).padStart(2, "0")}</span><div><div className="release-summary-meta"><strong>v{release.version}</strong><span><CalendarDays size={14} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "long" }).format(new Date(release.published_at))}</span></div><h2>{release.summary}</h2><div className="release-platforms">{[...new Set(release.artifacts.map((item) => item.platform))].map((platform) => <span className="vlt-badge" key={platform}>{platform === "macos" ? "macOS" : platform === "windows" ? "Windows" : "Linux"}</span>)}</div></div><Link className="release-open" href={`/${locale}/releases/${release.version}`} aria-label={`${ru ? "Открыть версию" : "Open version"} ${release.version}`}><ArrowRight size={20} aria-hidden /></Link></article>)}</div>}
  </main>;
}
