import { CheckCircle2, GitCommitHorizontal, Sparkles } from "lucide-react";
import type { PublicRelease } from "@/lib/releases";

export function ReleaseNotes({ release, locale }: { release: PublicRelease; locale: string }) {
  const ru = locale === "ru";
  const groups = [
    [ru ? "Новые функции" : "New features", release.features, Sparkles],
    [ru ? "Изменения" : "Changes", release.changes, GitCommitHorizontal],
    [ru ? "Исправления" : "Bug fixes", release.fixes, CheckCircle2],
  ] as const;
  const visibleGroups = groups.filter(([, items]) => items.length > 0);
  if (visibleGroups.length === 0) return null;

  return <section className="release-notes" aria-label={ru ? "Изменения в версии" : "Changes in this version"} data-reveal>
    <header className="release-notes-heading">
      <span className="section-label">{ru ? "Что нового" : "Release notes"}</span>
      <h2>{ru ? `Изменения в версии ${release.version}` : `What changed in ${release.version}`}</h2>
    </header>
    <div className="release-note-groups">
      {visibleGroups.map(([title, items, Icon]) => <div className="release-note-group" key={title}>
        <header><span><Icon size={18} aria-hidden /></span><h3>{title}</h3></header>
        <ul>{items.map((item, index) => <li key={`${index}-${item}`}><span>{String(index + 1).padStart(2, "0")}</span><p>{item}</p></li>)}</ul>
      </div>)}
    </div>
  </section>;
}
