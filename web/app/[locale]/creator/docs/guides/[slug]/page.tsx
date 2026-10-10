import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { setRequestLocale } from "next-intl/server";
import { siteMetadata } from "@/lib/seo";
import { findGuide, guides } from "@/lib/creator/guides";
import { findNode, portTypes } from "@/lib/creator/catalog";
import { CreatorDocsLayout } from "@/components/creator/docs-layout";
import { TremoloDiagram } from "@/components/creator/tremolo-diagram";
import { PortExplorer } from "@/components/creator/port-explorer";
import { MemoryDemo } from "@/components/creator/memory-demo";
import { ModulationLab } from "@/components/creator/modulation-lab";
import type { Locale } from "@/lib/creator/types";

type Params = Promise<{ locale: Locale; slug: string }>;
export function generateStaticParams() { return guides.map(g => ({ slug: g.slug })); }
export async function generateMetadata({ params }: { params: Params }): Promise<Metadata> {
  const { locale, slug } = await params; const guide = findGuide(slug); if (!guide) notFound();
  return siteMetadata(locale, `/creator/docs/guides/${slug}`, `${guide.title[locale]} — Creator`, guide.intro[locale]);
}
export default async function GuidePage({ params }: { params: Params }) {
  const { locale, slug } = await params; setRequestLocale(locale);
  const guide = findGuide(slug); if (!guide) notFound();
  const next = guides[(guides.indexOf(guide) + 1) % guides.length];
  return <CreatorDocsLayout locale={locale} active={slug} animated={guide.sections.some(section => section.diagram || section.id === "feedback")} toc={guide.sections.map(s => ({ id: s.id, title: s.title[locale] }))}>
    <article className="creator-article"><header className="creator-doc-heading"><span className="creator-eyebrow">CREATOR / {locale === "ru" ? "РУКОВОДСТВО" : "GUIDE"}</span><h1>{guide.title[locale]}</h1><p>{guide.intro[locale]}</p></header>
    {slug === "ports" && <PortExplorer types={portTypes} locale={locale} />}
    {slug === "modulation" && <ModulationLab locale={locale} />}
    {guide.sections.map(section => <section id={section.id} key={section.id}><h2>{section.title[locale]}</h2>{section.paragraphs?.map((p, i) => <p key={i}>{p[locale]}</p>)}{section.steps && <ol className="creator-steps">{section.steps.map((step, i) => <li key={i}>{step[locale]}</li>)}</ol>}{section.code && <pre><code>{section.code}</code></pre>}{section.diagram && <TremoloDiagram locale={locale} />}{section.id === "feedback" && <MemoryDemo locale={locale} />}{section.links && <nav className="creator-related" aria-label={locale === "ru" ? "Ноды из примера" : "Nodes in this example"}>{section.links.map(id => <Link key={id} href={`/creator/docs/nodes/${id}`}>{findNode(id)!.name}<span aria-hidden>↗</span></Link>)}</nav>}</section>)}
    {slug === "cpp" && <p className="creator-callout"><a href="/creator/downloads/creator-sdk-ru.md" download>{locale === "ru" ? "Скачать полный справочник SDK на русском: API, жизненный цикл и исходники примеров" : "Download the complete Russian SDK reference: API, lifecycle and example source"}</a></p>}
    {slug === "first-effect" && <p className="creator-callout"><a href="/creator/downloads/Gentle-Tremolo.vltcreator" download>{locale === "ru" ? "Скачать редактируемый проект Gentle Tremolo" : "Download the editable Gentle Tremolo project"}</a></p>}
    <Link className="creator-next-guide" href={`/creator/docs/guides/${next.slug}`}>{locale === "ru" ? "Далее" : "Next"}<strong>{next.title[locale]} →</strong></Link></article>
  </CreatorDocsLayout>;
}
