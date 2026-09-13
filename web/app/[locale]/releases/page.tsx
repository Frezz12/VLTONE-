import Image from "next/image";
import Link from "next/link";
import { ArrowRight, CalendarDays, PackageOpen, Sparkles } from "lucide-react";
import { ReleaseDownloads } from "@/components/release-downloads";
import { ReleaseNotes } from "@/components/release-notes";
import { getReleases } from "@/lib/releases";
import { siteMetadata } from "@/lib/seo";

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  return siteMetadata(locale, "/releases", locale === "ru" ? "Скачать VLTone — последняя версия" : "Download VLTone — latest version", locale === "ru" ? "Скачайте последнюю версию VLTone для своей системы и посмотрите список изменений." : "Download the latest VLTone version for your system and read the release notes.");
}

export const dynamic = "force-dynamic";

export default async function ReleasesPage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const releases = await getReleases(locale);
  const ru = locale === "ru";
  const latest = releases[0];
  return <main id="main-content" className="releases-main">
    {!latest ? <section className="releases-empty"><PackageOpen size={28} aria-hidden /><h1>{ru ? "Релизов пока нет" : "No releases yet"}</h1><p>{ru ? "Первая опубликованная версия появится здесь." : "The first published version will appear here."}</p></section> : <>
      <header className="download-hero" data-reveal>
        <div className="download-hero-copy">
          <span className="release-eyebrow"><Sparkles size={15} aria-hidden />{ru ? `Последняя версия · ${latest.version}` : `Latest version · ${latest.version}`}</span>
          <h1>{ru ? "Скачать VLTone" : "Download VLTone"}</h1>
          <p>{latest.summary}</p>
          <span className="release-date"><CalendarDays size={15} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "long" }).format(new Date(latest.published_at))}</span>
          <div className="download-actions"><ReleaseDownloads artifacts={latest.artifacts} locale={locale} /></div>
          <p className="download-account-note">{ru ? "После установки войдите с аккаунтом VLTone." : "Sign in with your VLTone account after installation."} <Link href="/register">{ru ? "Создать аккаунт" : "Create an account"}<ArrowRight size={13} aria-hidden /></Link></p>
        </div>
        <figure className="download-preview">
          <div className="download-signal" aria-hidden="true">{[0, 1, 2, 3, 4, 5, 6].map((index) => <span key={index} />)}</div>
          <Image src={`/images/workspace-dark-${locale}.png`} width={1600} height={1000} sizes="(max-width: 900px) 94vw, 54vw" priority alt={ru ? "Интерфейс VLTone" : "VLTone interface"} />
          <figcaption>VLTone · Windows · macOS</figcaption>
        </figure>
      </header>

      <ReleaseNotes release={latest} locale={locale} />

      <section className="release-archive" aria-labelledby="release-archive-title">
        <header data-reveal><div><span className="section-label">{ru ? "Архив" : "Archive"}</span><h2 id="release-archive-title">{ru ? "Выбрать версию" : "Choose a version"}</h2></div><p>{ru ? "Откройте нужную версию, чтобы скачать её установщики и посмотреть полный список изменений." : "Open any version to download its installers and see the complete release notes."}</p></header>
        <div className="release-version-list">{releases.map((release, index) => <a className="release-version-row" href={`/releases/${release.version}`} key={release.id} data-reveal={String(index * 45)}>
          <span className="release-index">{String(index + 1).padStart(2, "0")}</span>
          <div><div className="release-summary-meta"><strong>v{release.version}</strong>{index === 0 && <span className="vlt-badge vlt-badge-accent">{ru ? "Последняя" : "Latest"}</span>}</div><h3>{release.summary}</h3></div>
          <span className="release-version-date"><CalendarDays size={14} aria-hidden />{new Intl.DateTimeFormat(locale, { dateStyle: "medium" }).format(new Date(release.published_at))}</span>
          <span className="release-open" aria-hidden="true"><ArrowRight size={19} /></span>
        </a>)}</div>
      </section>
    </>}
  </main>;
}
