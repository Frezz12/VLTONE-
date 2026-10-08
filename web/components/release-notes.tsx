import { CheckCircle2, GitCommitHorizontal, Plus } from "lucide-react";
import { ProductShot } from "@/components/product-gallery";
import { getReleaseHighlights } from "@/lib/release-highlights";
import type { PublicRelease } from "@/lib/releases";

export function ReleaseNotes({ release, locale }: { release: PublicRelease; locale: string }) {
  const ru = locale === "ru";
  const { highlights, changes, features, screenshots } = getReleaseHighlights(release, locale);
  const groups = [
    [ru ? "Другие новые функции" : "More new features", features ?? [], GitCommitHorizontal],
    [ru ? "Другие изменения" : "Other changes", changes, GitCommitHorizontal],
    [ru ? "Исправления" : "Bug fixes", release.fixes, CheckCircle2],
  ] as const;
  const visibleGroups = groups.filter(([, items]) => items.length > 0);
  if (!highlights.length && !visibleGroups.length && !screenshots.length) return null;

  return <section id="release-notes" className="release-notes" aria-label={ru ? "Изменения в версии" : "Changes in this version"}>
    <header className="release-notes-heading" data-reveal>
      <span className="section-label">{ru ? "Что нового" : "What's new"}</span>
      <h2>{ru ? `Новое в VLTone ${release.version}` : `New in VLTone ${release.version}`}</h2>
    </header>
    {highlights.length > 0 && <div className="release-highlights">
      {highlights.map(item => <article id={`update-${item.id}`} className="release-highlight" data-tone={item.tone} data-has-media={Boolean(item.screenshot)} key={item.id} aria-labelledby={`update-${item.id}-title`} data-reveal>
        <div className="release-highlight-copy">
          <span className="release-highlight-label">{item.label}</span>
          <h3 id={`update-${item.id}-title`}>{item.title}</h3>
          {item.lead && <p className="release-highlight-lead">{item.lead}</p>}
          {item.details.length > 0 && <details className="release-highlight-details">
            <summary><span className="release-details-open">{ru ? "Подробнее" : "Read more"}</span><span className="release-details-close">{ru ? "Свернуть" : "Show less"}</span><span className="sr-only">: {item.title}</span><Plus size={17} aria-hidden /></summary>
            <ul>{item.details.map((detail, index) => <li key={index}>{detail}</li>)}</ul>
          </details>}
        </div>
        {item.screenshot && <figure className="release-highlight-media">
          <ProductShot locale={locale} name={`release-${item.screenshot.id}`} alt={item.screenshot.caption} source={{ src: `/api${item.screenshot.url}`, width: item.screenshot.width, height: item.screenshot.height }} />
          <figcaption>{item.screenshot.caption}</figcaption>
        </figure>}
      </article>)}
    </div>}
    {visibleGroups.length > 0 && <div className="release-note-groups">
      {visibleGroups.map(([title, items, Icon]) => <section className="release-note-group" key={title}>
        <header><Icon size={20} aria-hidden /><h3>{title}</h3></header>
        <ul>{items.map((item, index) => <li key={index}>{item}</li>)}</ul>
      </section>)}
    </div>}
    {screenshots.length > 0 && <section className="release-gallery" aria-label={ru ? "Скриншоты версии" : "Release screenshots"}>
      <header><h3>{ru ? "Ещё о версии" : "More from this version"}</h3></header>
      <div>{screenshots.map(shot => <figure key={shot.id}>
        <ProductShot locale={locale} name={`release-${shot.id}`} alt={shot.caption} source={{ src: `/api${shot.url}`, width: shot.width, height: shot.height }} />
        <figcaption>{shot.caption}</figcaption>
      </figure>)}</div>
    </section>}
  </section>;
}
