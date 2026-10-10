import Link from "next/link";
import { ArrowLeft, ArrowUpRight, BookOpen } from "lucide-react";
import { guides } from "@/lib/creator/guides";
import { creatorVersion } from "@/lib/creator/catalog";
import { AmbientMotionControl } from "@/components/ambient-motion-control";
import type { Locale } from "@/lib/creator/types";

export function CreatorDocsLayout({ locale, active, children, toc = [], animated = false }: { locale: Locale; active?: string; children: React.ReactNode; toc?: { id: string; title: string }[]; animated?: boolean }) {
  const ru = locale === "ru";
  const nav = <nav aria-label={ru ? "Документация Creator" : "Creator documentation"}>
    <Link className="creator-doc-home" href="/creator/docs" aria-current={active === "index" ? "page" : undefined}><BookOpen size={17} aria-hidden />{ru ? "Обзор и поиск" : "Overview & search"}</Link>
    <span className="creator-small-label">{ru ? "Руководства" : "Guides"}</span>
    {guides.map((guide, index) => <Link key={guide.slug} href={`/creator/docs/guides/${guide.slug}`} aria-current={active === guide.slug ? "page" : undefined}><small>{String(index + 1).padStart(2, "0")}</small>{guide.title[locale]}</Link>)}
    <Link className="creator-doc-home" href="/creator/docs#catalog">{ru ? "Справочник всех нод" : "Complete node reference"}<ArrowUpRight size={15} aria-hidden /></Link>
  </nav>;
  return <main id="main-content" className="creator-docs creator-theme" data-motion-root data-motion-paused="true" data-page-visible="false">
    <div className="creator-docs-top"><Link href="/creator"><ArrowLeft size={15} aria-hidden />Creator</Link><span>{ru ? "Документация" : "Documentation"} <code>{creatorVersion}</code></span></div>
    <details className="creator-mobile-nav"><summary>{ru ? "Разделы документации" : "Documentation sections"}</summary>{nav}{!!toc.length && <><span className="creator-small-label">{ru ? "В этой статье" : "In this article"}</span><nav aria-label={ru ? "Оглавление статьи" : "Article contents"}>{toc.map(item => <a href={`#${item.id}`} key={item.id}>{item.title}</a>)}</nav></>}</details>
    <div className="creator-docs-layout"><aside className="creator-docs-sidebar">{nav}</aside><div className="creator-docs-content">{children}{animated && <div className="creator-docs-motion"><AmbientMotionControl locale={locale} /></div>}</div>
    {!!toc.length && <aside className="creator-article-toc"><span className="creator-small-label">{ru ? "На этой странице" : "On this page"}</span><nav aria-label={ru ? "Оглавление статьи" : "Article contents"}>{toc.map(item => <a href={`#${item.id}`} key={item.id}>{item.title}</a>)}</nav></aside>}</div>
  </main>;
}
