"use client";

import Image from "next/image";
import {
  AudioLines,
  Bot,
  ChevronRight,
  CircleHelp,
  FolderKanban,
  Maximize2,
  Piano,
  Rocket,
  Search,
  SlidersHorizontal,
  X,
} from "lucide-react";
import { useTranslations } from "next-intl";
import { KeyboardEvent, useEffect, useMemo, useRef, useState } from "react";

type Figure = { slug: string; alt: string; caption: string; ratio?: string };
type Shortcut = { keys: string; label: string };
type Problem = { problem: string; fix: string };
type Chapter = {
  id: string;
  title: string;
  intro: string;
  preparation?: string;
  steps: string[];
  result: string;
  shortcuts?: Shortcut[];
  notes?: string[];
  problems?: Problem[];
  figures?: Figure[];
};
type Category = { id: string; label: string; summary: string; chapters: Chapter[] };

const icons = {
  start: Rocket,
  project: FolderKanban,
  audio: AudioLines,
  midi: Piano,
  mix: SlidersHorizontal,
  ai: Bot,
  help: CircleHelp,
} as const;

function chapterMatches(chapter: Chapter, query: string, locale: string) {
  if (!query) return true;
  return JSON.stringify(chapter).toLocaleLowerCase(locale).includes(query);
}

export function ManualView({ locale }: { locale: "ru" | "en" }) {
  const t = useTranslations("Manual");
  const categories = useMemo(() => t.raw("categories") as Category[], [t]);
  const [activeId, setActiveId] = useState(categories[0].id);
  const [search, setSearch] = useState("");
  const [figure, setFigure] = useState<Figure | null>(null);
  const dialogRef = useRef<HTMLDialogElement>(null);
  const tabRefs = useRef<Array<HTMLButtonElement | null>>([]);

  const query = search.trim().toLocaleLowerCase(locale);
  const visible = useMemo(() => categories
    .map((category) => ({ ...category, chapters: category.chapters.filter((chapter) => chapterMatches(chapter, query, locale)) }))
    .filter((category) => category.chapters.length > 0), [categories, locale, query]);
  const shown = query ? visible : categories.filter((category) => category.id === activeId);
  const resultCount = visible.reduce((sum, category) => sum + category.chapters.length, 0);

  useEffect(() => {
    const openHash = () => {
      const id = decodeURIComponent(window.location.hash.slice(1));
      if (!id) return;
      const owner = categories.find((category) => category.chapters.some((chapter) => chapter.id === id));
      if (!owner) return;
      setSearch("");
      setActiveId(owner.id);
      window.requestAnimationFrame(() => document.getElementById(id)?.scrollIntoView());
    };
    openHash();
    window.addEventListener("hashchange", openHash);
    return () => window.removeEventListener("hashchange", openHash);
  }, [categories]);

  useEffect(() => {
    const dialog = dialogRef.current;
    if (!dialog) return;
    if (figure && !dialog.open) dialog.showModal();
    if (!figure && dialog.open) dialog.close();
  }, [figure]);

  function selectTab(id: string) {
    setSearch("");
    setActiveId(id);
    document.getElementById("manual-content")?.scrollIntoView({ block: "start" });
  }

  function onTabKeyDown(event: KeyboardEvent<HTMLButtonElement>, index: number) {
    if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const next = event.key === "Home" ? 0 : event.key === "End" ? categories.length - 1
      : (index + (event.key === "ArrowRight" ? 1 : -1) + categories.length) % categories.length;
    selectTab(categories[next].id);
    tabRefs.current[next]?.focus();
  }

  const toc = query ? visible : categories;

  return <main className="manual-main">
    <section className="manual-hero" aria-labelledby="manual-title">
      <div>
        <span className="manual-eyebrow">{t("eyebrow")}</span>
        <h1 id="manual-title">{t("title")}</h1>
        <p>{t("intro")}</p>
      </div>
      <label className="manual-search">
        <span>{t("searchLabel")}</span>
        <span className="manual-search-field"><Search size={19} aria-hidden /><input value={search} onChange={(event) => setSearch(event.target.value)} placeholder={t("searchPlaceholder")} type="search" /></span>
        <small>{query ? t("resultCount", { count: resultCount }) : t("searchHint")}</small>
      </label>
    </section>

    <div className="manual-tabs" role="tablist" aria-label={t("categoryLabel")}>
      {categories.map((category, index) => {
        const Icon = icons[category.id as keyof typeof icons];
        return <button
          key={category.id}
          ref={(node) => { tabRefs.current[index] = node; }}
          id={`tab-${category.id}`}
          type="button"
          role="tab"
          aria-selected={activeId === category.id}
          aria-controls={`panel-${category.id}`}
          tabIndex={activeId === category.id ? 0 : -1}
          onClick={() => selectTab(category.id)}
          onKeyDown={(event) => onTabKeyDown(event, index)}
        ><Icon size={17} aria-hidden /><span>{category.label}</span></button>;
      })}
    </div>

    <details className="manual-mobile-toc">
      <summary>{t("toc")}</summary>
      <Toc categories={toc} />
    </details>

    <div className="manual-layout" id="manual-content">
      <aside className="manual-sidebar" aria-label={t("toc")}><span>{t("toc")}</span><Toc categories={toc} /></aside>
      <div className="manual-content">
        {shown.length === 0 ? <div className="manual-empty" role="status"><Search size={25} aria-hidden /><h2>{t("noResultsTitle")}</h2><p>{t("noResults")}</p><button type="button" onClick={() => setSearch("")}>{t("clearSearch")}</button></div> : shown.map((category) => <section
          className="manual-category"
          id={`panel-${category.id}`}
          key={category.id}
          role={query ? undefined : "tabpanel"}
          aria-labelledby={query ? undefined : `tab-${category.id}`}
        >
          <header className="manual-category-head"><span>{String(categories.findIndex((item) => item.id === category.id) + 1).padStart(2, "0")}</span><div><h2>{category.label}</h2><p>{category.summary}</p></div></header>
          {category.chapters.map((chapter, index) => <article className="manual-chapter" id={chapter.id} key={chapter.id}>
            <header><span>{String(index + 1).padStart(2, "0")}</span><div><h3>{chapter.title}</h3><p>{chapter.intro}</p></div></header>
            {chapter.preparation && <div className="manual-callout"><strong>{t("preparation")}</strong><p>{chapter.preparation}</p></div>}
            <div className="manual-section"><h4>{t("steps")}</h4><ol>{chapter.steps.map((step, stepIndex) => <li key={step}><span>{stepIndex + 1}</span><p>{step}</p></li>)}</ol></div>
            <div className="manual-result"><strong>{t("result")}</strong><p>{chapter.result}</p></div>
            {chapter.shortcuts?.length ? <div className="manual-section"><h4>{t("shortcuts")}</h4><div className="manual-shortcuts">{chapter.shortcuts.map((shortcut) => <div key={`${shortcut.keys}-${shortcut.label}`}><kbd>{shortcut.keys}</kbd><span>{shortcut.label}</span></div>)}</div></div> : null}
            {chapter.notes?.length ? <div className="manual-section"><h4>{t("notes")}</h4><ul className="manual-notes">{chapter.notes.map((note) => <li key={note}>{note}</li>)}</ul></div> : null}
            {chapter.problems?.length ? <div className="manual-section"><h4>{t("problems")}</h4><div className="manual-problems">{chapter.problems.map((problem) => <div key={problem.problem}><strong>{problem.problem}</strong><p>{problem.fix}</p></div>)}</div></div> : null}
            {chapter.figures?.length ? <div className="manual-gallery">{chapter.figures.map((item) => <figure key={item.slug} style={{ "--shot-ratio": item.ratio ?? "6 / 5" } as React.CSSProperties}>
              <button type="button" onClick={() => setFigure(item)} aria-label={`${t("openImage")}: ${item.alt}`}>
                <Image fill sizes="(max-width: 900px) 100vw, 820px" src={`/manual/${locale}/${item.slug}.png`} alt={item.alt} />
                <span><Maximize2 size={16} aria-hidden />{t("openImage")}</span>
              </button>
              <figcaption>{item.caption}</figcaption>
            </figure>)}</div> : null}
          </article>)}
        </section>)}
      </div>
    </div>

    <dialog className="manual-lightbox" ref={dialogRef} onClose={() => setFigure(null)} onClick={(event) => { if (event.target === event.currentTarget) setFigure(null); }}>
      {figure && <div><button className="manual-lightbox-close" type="button" onClick={() => setFigure(null)} aria-label={t("closeImage")}><X size={20} aria-hidden /></button><div className="manual-lightbox-image"><Image fill sizes="96vw" src={`/manual/${locale}/${figure.slug}.png`} alt={figure.alt} /></div><p>{figure.caption}</p></div>}
    </dialog>
  </main>;
}

function Toc({ categories }: { categories: Category[] }) {
  return <nav>{categories.map((category) => <div key={category.id}><strong>{category.label}</strong>{category.chapters.map((chapter) => <a key={chapter.id} href={`#${chapter.id}`}><ChevronRight size={13} aria-hidden />{chapter.title}</a>)}</div>)}</nav>;
}
