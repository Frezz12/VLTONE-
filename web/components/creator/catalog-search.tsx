"use client";

import Link from "next/link";
import { useMemo, useState } from "react";
import { ArrowUpRight, Search } from "lucide-react";
import type { Locale } from "@/lib/creator/types";

export type SearchNode = { id: string; name: string; category: string; categoryLabel: string; summary: string; terms: string };
export function CatalogSearch({ nodes, categories, guides, locale, initialCategory = "", initialQuery = "" }: { nodes: SearchNode[]; categories: { id: string; label: string }[]; guides: { slug: string; title: string; terms: string }[]; locale: Locale; initialCategory?: string; initialQuery?: string }) {
  const [query, setQuery] = useState(initialQuery);
  const [category, setCategory] = useState(initialCategory);
  const [expanded, setExpanded] = useState(false);
  const normalized = query.trim().toLocaleLowerCase(locale);
  const filtered = useMemo(() => nodes.filter(node => (!category || node.category === category) && (!normalized || node.terms.toLocaleLowerCase(locale).includes(normalized))), [nodes, category, normalized, locale]);
  const matchedGuides = normalized ? guides.filter(guide => guide.terms.toLocaleLowerCase(locale).includes(normalized)) : [];
  const shown = expanded || normalized || category ? filtered : filtered.slice(0, 24);
  const ru = locale === "ru";
  function updateUrl(q: string, cat: string) {
    const url = new URL(window.location.href);
    if (q) url.searchParams.set("q", q); else url.searchParams.delete("q");
    if (cat) url.searchParams.set("category", cat); else url.searchParams.delete("category");
    window.history.replaceState(null, "", url);
  }
  return <div className="creator-catalog" id="catalog">
    <div className="creator-search-row">
      <label className="creator-search"><span>{ru ? "Поиск по документации" : "Search documentation"}</span><div><Search size={19} aria-hidden /><input type="search" placeholder={ru ? "Нода, порт или параметр…" : "Node, port or parameter…"} value={query} onChange={event => { setQuery(event.target.value); updateUrl(event.target.value, category); }} /></div></label>
      <label className="creator-category-select"><span>{ru ? "Категория" : "Category"}</span><select value={category} onChange={event => { setCategory(event.target.value); updateUrl(query, event.target.value); }}><option value="">{ru ? "Все категории" : "All categories"}</option>{categories.map(item => <option key={item.id} value={item.id}>{item.label}</option>)}</select></label>
    </div>
    <p className="creator-search-count" role="status">{ru ? `Найдено нод: ${filtered.length}` : `${filtered.length} nodes found`}{matchedGuides.length ? (ru ? ` · Руководств: ${matchedGuides.length}` : ` · ${matchedGuides.length} guides`) : ""}</p>
    {!!matchedGuides.length && <nav className="creator-guide-results" aria-label={ru ? "Найденные руководства" : "Matching guides"}>{matchedGuides.map(guide => <Link key={guide.slug} href={`/creator/docs/guides/${guide.slug}`}>{guide.title}<ArrowUpRight size={16} aria-hidden /></Link>)}</nav>}
    <div className="creator-node-grid">{shown.map(node => <Link className="creator-catalog-node" href={`/creator/docs/nodes/${node.id}`} key={node.id}><span className="creator-small-label">{node.categoryLabel}</span><h3>{node.name}<ArrowUpRight size={16} aria-hidden /></h3><p>{node.summary}</p><code>{node.id}</code></Link>)}</div>
    {!filtered.length && <div className="creator-empty"><h3>{ru ? "Ноды не найдены" : "No matching nodes"}</h3><p>{ru ? "Попробуй название на английском, ID или другой тип порта." : "Try an English name, an ID or another port type."}</p><button type="button" onClick={() => { setQuery(""); setCategory(""); updateUrl("", ""); }}>{ru ? "Сбросить поиск" : "Clear search"}</button></div>}
    {shown.length < filtered.length && <button type="button" className="creator-load-more" onClick={() => setExpanded(true)}>{ru ? `Показать все ${filtered.length} нод` : `Show all ${filtered.length} nodes`}<ArrowUpRight size={17} aria-hidden /></button>}
  </div>;
}
