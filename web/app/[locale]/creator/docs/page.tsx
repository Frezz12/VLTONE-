import type { Metadata } from "next";
import Link from "next/link";
import { ArrowRight, ArrowUpRight } from "lucide-react";
import { setRequestLocale } from "next-intl/server";
import { siteMetadata } from "@/lib/seo";
import { creatorNodes, categories } from "@/lib/creator/catalog";
import { guides } from "@/lib/creator/guides";
import { CatalogSearch } from "@/components/creator/catalog-search";
import { CreatorDocsLayout } from "@/components/creator/docs-layout";
import { TremoloDiagram } from "@/components/creator/tremolo-diagram";
import type { Locale } from "@/lib/creator/types";

export async function generateMetadata({ params }: { params: Promise<{ locale: Locale }> }): Promise<Metadata> {
  const { locale } = await params;
  return siteMetadata(locale, "/creator/docs", locale === "ru" ? "Документация Creator — VLTone" : "Creator documentation — VLTone", locale === "ru" ? "Руководства по Creator, порты, соединения, C++ и полный справочник нод с параметрами и примерами." : "Creator guides, ports, connections, C++ and the complete node reference with parameters and examples.");
}
export default async function DocsPage({ params, searchParams }: { params: Promise<{ locale: Locale }>; searchParams: Promise<{ category?: string; q?: string }> }) {
  const { locale } = await params; setRequestLocale(locale);
  const query = await searchParams;
  const ru = locale === "ru";
  const items = creatorNodes.map(node => ({ id: node.id, name: node.name, category: node.category, categoryLabel: categories[node.category][locale], summary: node.summary[locale], terms: [node.id, node.name, categories[node.category][locale], node.summary[locale], node.example[locale], ...node.inputs.flatMap(p => [p.id, p.name, p.type]), ...node.outputs.flatMap(p => [p.id, p.name, p.type]), ...node.parameters.flatMap(p => [p.id, p.name, p.unit])].join(" ") }));
  return <CreatorDocsLayout locale={locale} active="index" animated>
    <header className="creator-doc-heading"><span className="creator-eyebrow">CREATOR / {ru ? "ДОКУМЕНТАЦИЯ" : "DOCUMENTATION"}</span><h1>{ru ? "От первой связи" : "From your first wire"}<br /><em>{ru ? "до своего эффекта." : "to your own effect."}</em></h1><p>{ru ? "Пойми, что проходит по проводам. Собери первую схему. Найди точные параметры каждой ноды." : "Understand what the wires carry. Build your first graph. Find the exact parameters of every node."}</p></header>
    <Link className="creator-quickstart" href="/creator/docs/guides/first-effect"><span><small>{ru ? "НАЧНИ ЗДЕСЬ" : "START HERE"}</small><strong>Gentle Tremolo</strong><span>{ru ? "Шесть нод. Две ручки. Твой первый эффект." : "Six nodes. Two controls. Your first effect."}</span></span><ArrowRight size={28} aria-hidden /></Link>
    <TremoloDiagram locale={locale} />
    <section className="creator-doc-guides" aria-labelledby="guides-title"><h2 id="guides-title">{ru ? "Разобраться в деталях" : "Get into the details"}</h2><div>{guides.slice(1).map((guide, i) => <Link key={guide.slug} href={`/creator/docs/guides/${guide.slug}`}><span>{String(i + 2).padStart(2, "0")}</span><strong>{guide.title[locale]}</strong><ArrowUpRight size={16} aria-hidden /></Link>)}</div></section>
    <section className="creator-doc-catalog"><header><span className="creator-eyebrow">{creatorNodes.length} {ru ? "ТИПОВ НОД" : "NODE TYPES"}</span><h2>{ru ? "У каждой ноды — своя задача." : "Every node has a purpose."}</h2><p>{ru ? "Названия и ID соответствуют редактору. Порты, диапазоны и значения по умолчанию взяты из нативного реестра." : "Names and IDs match the editor. Ports, ranges and defaults come from the native registry."}</p></header>
      <CatalogSearch nodes={items} categories={Object.entries(categories).map(([id, label]) => ({ id, label: label[locale] }))} locale={locale} initialCategory={typeof query.category === "string" && categories[query.category] ? query.category : ""} initialQuery={typeof query.q === "string" ? query.q : ""} guides={guides.map(g => ({ slug: g.slug, title: g.title[locale], terms: [g.title[locale], g.intro[locale], ...g.sections.flatMap(s => [s.title[locale], ...(s.paragraphs ?? []).map(p => p[locale]), ...(s.steps ?? []).map(p => p[locale])])].join(" ") }))} />
    </section>
  </CreatorDocsLayout>;
}
