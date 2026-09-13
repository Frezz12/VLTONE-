import Image from "next/image";
import { ArrowLeft, CalendarDays } from "lucide-react";
import { notFound } from "next/navigation";
import { ReleaseDownloads } from "@/components/release-downloads";
import { ReleaseNotes } from "@/components/release-notes";
import { getRelease } from "@/lib/releases";
import { siteMetadata } from "@/lib/seo";

export async function generateMetadata({ params }: { params: Promise<{ locale: string; version: string }> }) {
  const { locale, version } = await params;
  const release = await getRelease(locale, version);
  if (!release) notFound();
  return siteMetadata(locale, `/releases/${encodeURIComponent(version)}`, `VLTone ${release.version} — ${locale === "ru" ? "скачать обновление" : "download update"}`, release.summary);
}

export const dynamic = "force-dynamic";

export default async function ReleasePage({ params }: { params: Promise<{ locale: string; version: string }> }) {
  const { locale, version } = await params;
  const release = await getRelease(locale, version);
  if (!release) notFound();
  const ru = locale === "ru";
  return <main id="main-content" className="release-detail-main">
    <a className="release-back" href="/releases"><ArrowLeft size={16} aria-hidden />{ru ? "Назад к загрузке" : "Back to downloads"}</a>
    <header className="release-detail-hero"><div><span className="release-eyebrow">VLTone</span><h1>v{release.version}</h1><p>{release.summary}</p><span className="release-date"><CalendarDays size={15} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "long" }).format(new Date(release.published_at))}</span></div><aside><h2>{ru ? "Скачать" : "Download"}</h2><p>{ru ? "Показаны только готовые файлы." : "Only available builds are shown."}</p><ReleaseDownloads artifacts={release.artifacts} locale={locale} /></aside></header>
    <ReleaseNotes release={release} locale={locale} />
    {release.screenshots.length > 0 && <section className="release-gallery"><header><span>{ru ? "Скриншоты" : "Screenshots"}</span><h2>{ru ? "Что изменилось визуально" : "What changed visually"}</h2></header><div>{release.screenshots.map((shot) => <figure key={shot.id}><a href={`/api${shot.url}`} target="_blank" rel="noreferrer"><Image src={`/api${shot.url}`} width={shot.width} height={shot.height} alt={shot.caption} unoptimized /></a><figcaption>{shot.caption}</figcaption></figure>)}</div></section>}
  </main>;
}
