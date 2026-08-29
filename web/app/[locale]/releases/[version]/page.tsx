import Image from "next/image";
import Link from "next/link";
import { ArrowLeft, CalendarDays, CheckCircle2, GitCommitHorizontal, Sparkles } from "lucide-react";
import { notFound } from "next/navigation";
import { ReleaseDownloads } from "@/components/release-downloads";
import { getRelease } from "@/lib/releases";

export const dynamic = "force-dynamic";

export default async function ReleasePage({ params }: { params: Promise<{ locale: string; version: string }> }) {
  const { locale, version } = await params;
  const release = await getRelease(locale, version);
  if (!release) notFound();
  const ru = locale === "ru";
  const sections = [
    [ru ? "Новое" : "New", release.features, Sparkles],
    [ru ? "Изменения" : "Changes", release.changes, GitCommitHorizontal],
    [ru ? "Исправления" : "Fixes", release.fixes, CheckCircle2],
  ] as const;
  return <main className="release-detail-main">
    <Link className="release-back" href={`/${locale}/releases`}><ArrowLeft size={16} aria-hidden />{ru ? "Все обновления" : "All releases"}</Link>
    <header className="release-detail-hero"><div><span className="release-eyebrow">VLT Studio Pro</span><h1>v{release.version}</h1><p>{release.summary}</p><span className="release-date"><CalendarDays size={15} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "long" }).format(new Date(release.published_at))}</span></div><aside><h2>{ru ? "Скачать" : "Download"}</h2><p>{ru ? "Показаны только готовые файлы." : "Only available builds are shown."}</p><ReleaseDownloads artifacts={release.artifacts} locale={locale} /></aside></header>
    <div className="release-notes">{sections.filter(([, items]) => items.length > 0).map(([title, items, Icon]) => <section key={title}><header><Icon size={19} aria-hidden /><h2>{title}</h2></header><ul>{items.map((item, index) => <li key={`${index}-${item}`}><span>{String(index + 1).padStart(2, "0")}</span><p>{item}</p></li>)}</ul></section>)}</div>
    {release.screenshots.length > 0 && <section className="release-gallery"><header><span>{ru ? "Скриншоты" : "Screenshots"}</span><h2>{ru ? "Что изменилось визуально" : "What changed visually"}</h2></header><div>{release.screenshots.map((shot) => <figure key={shot.id}><a href={`/api${shot.url}`} target="_blank" rel="noreferrer"><Image src={`/api${shot.url}`} width={shot.width} height={shot.height} alt={shot.caption} unoptimized /></a><figcaption>{shot.caption}</figcaption></figure>)}</div></section>}
  </main>;
}
