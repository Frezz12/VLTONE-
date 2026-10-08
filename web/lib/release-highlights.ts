import type { PublicRelease, ReleaseScreenshot } from "@/lib/releases";

const releaseTones = ["coral", "lilac", "lime", "sky", "peach", "mint", "rose", "periwinkle"] as const;

type Highlight = {
  id: string;
  title: string;
  lead: string;
  label: string;
  tone: typeof releaseTones[number];
  details: string[];
  screenshot?: ReleaseScreenshot;
};

type HighlightPlan = {
  id: string;
  title: string;
  lead: string;
  label: string;
  features?: number[];
  changes?: number[];
  shot: number;
};

// Editorial groups for the published release. The full API notes remain in each disclosure.
function plans031(ru: boolean): HighlightPlan[] {
  return [
    {
      id: "slide-notes", label: ru ? "Мелодии" : "Melodies", shot: 20, features: [0, 1, 2, 3, 4, 6],
      title: ru ? "Рисуемые слайд-ноты" : "Draw your pitch transitions",
      lead: ru ? "Создавай переходы высоты внутри партии. Рисуй кривые, меняй форму и слушай результат без повторной атаки звука." : "Shape pitch transitions inside a phrase. Draw curves, change their shape and hear the result without retriggering the sound.",
    },
    {
      id: "sampler-legato", label: ru ? "Инструменты" : "Instruments", shot: 60, features: [5, 7],
      title: ru ? "Slide и Legato в Sampler" : "Slide and Legato in Sampler",
      lead: ru ? "Плавно соединяй ноты и меняй сэмпл прямо из Piano Roll. Настройки инструмента сохраняются." : "Connect overlapping notes smoothly and replace a sample directly from Piano Roll while keeping your instrument settings.",
    },
    {
      id: "chord-generator", label: ru ? "Аккорды" : "Chords", shot: 30, features: [8],
      title: ru ? "Аккорды с пустого клипа" : "Start a chord from an empty clip",
      lead: ru ? "Выбери основную ноту, тип аккорда и обращение. Генератор создаст партию даже там, где ещё нет нот." : "Choose a root, chord type and inversion. The generator can create a chord even when your clip has no notes yet.",
    },
    {
      id: "master-automation", label: ru ? "Сведение" : "Mixing", shot: 70, features: [9],
      title: ru ? "Автоматизация Master" : "Automate your Master channel",
      lead: ru ? "Управляй громкостью, панорамой и параметрами мастер-плагинов с помощью кривых на таймлайне." : "Shape volume, pan and master plugin parameters with automation curves on the timeline.",
    },
    {
      id: "bounce", label: ru ? "Аранжировка" : "Arrangement", shot: 80, features: [10, 11],
      title: ru ? "Общий Bounce дорожек" : "Bounce several tracks together",
      lead: ru ? "Собери выбранные дорожки в один аудиоклип. Сворачивай длинный проект, сохраняя обзор его содержимого." : "Render selected tracks into one audio clip. Fold a long session into compact tracks while keeping its contents in view.",
    },
    {
      id: "piano-roll", label: ru ? "Редактор нот" : "Note editing", shot: 10, changes: [0, 1, 2, 5],
      title: ru ? "Piano Roll под рукой" : "A clearer Piano Roll",
      lead: ru ? "Выбирай MIDI-клип, управляй масштабом и редактируй ноты в обновлённой рабочей области. Во время воспроизведения можно свободно прокручивать партию." : "Choose a MIDI clip, adjust the zoom and edit notes in the updated workspace. Scroll freely through your phrase during playback.",
    },
    {
      id: "render", label: ru ? "Экспорт" : "Export", shot: 40, changes: [3, 7],
      title: ru ? "Экспорт на одном экране" : "Your export on one screen",
      lead: ru ? "Формат, диапазон, обработка, стемы и метаданные собраны вместе. Сводка и прогресс остаются перед глазами." : "Format, range, processing, stems and metadata are together in one place. The summary and progress stay within reach.",
    },
    {
      id: "ai-chat", label: ru ? "AI-помощник" : "AI assistant", shot: 50, changes: [4],
      title: ru ? "AI-чат в стиле программы" : "A chat that fits your workspace",
      lead: ru ? "Обновлённые сообщения, поле ввода и понятные действия с подсказками. Всё оформлено в общем стиле VLTone." : "Updated messages, input and clear actions with tooltips, all styled to match the VLTone workspace.",
    },
  ];
}

function splitNote(text: string) {
  const sentence = text.match(/^(.+?[.!?])(?:\s+|$)([\s\S]*)$/);
  const first = sentence?.[1] ?? text;
  const prefix = first.match(/^(.{3,70}?)(?:[:]| — )\s+([\s\S]+)$/);
  if (prefix) return { title: prefix[1], lead: prefix[2], details: sentence?.[2] ? [sentence[2]] : [] };
  if (first.length <= 110) {
    const rest = sentence?.[2] ?? "";
    const next = rest.match(/^(.+?[.!?])(?:\s+|$)([\s\S]*)$/);
    return { title: first, lead: next?.[1] ?? rest, details: next?.[2] ? [next[2]] : [] };
  }
  const title = first.slice(0, 70).replace(/\s+\S*$/, "") + "…";
  return { title, lead: "", details: [text] };
}

const ignoredWords = new Set(["added", "new", "updated", "новый", "новая", "новые", "добавл", "обновл", "работа", "проект", "интерф", "vltone", "vlt"]);
function words(text: string) {
  return new Set((text.toLowerCase().match(/[a-zа-яё]{4,}/g) ?? []).map(word => word.slice(0, 6)).filter(word => !ignoredWords.has(word)));
}

function matchingScreenshot(text: string, screenshots: ReleaseScreenshot[]) {
  const terms = words(text);
  const matches = screenshots.map(screenshot => ({ screenshot, score: [...words(screenshot.caption)].filter(word => terms.has(word)).length }));
  matches.sort((a, b) => b.score - a.score);
  return matches[0]?.score > 0 ? matches[0].screenshot : undefined;
}

export function getReleaseHighlights(release: PublicRelease, locale: string) {
  const ru = locale === "ru";
  if (release.highlights?.length) {
    const used = new Set<string>();
    const highlights: Highlight[] = release.highlights.map(item => {
      const screenshot = release.screenshots.find(shot => shot.id === item.screenshot_id);
      if (screenshot) used.add(screenshot.id);
      return { id: item.id, tone: item.tone, title: ru ? item.title_ru : item.title_en, lead: ru ? item.lead_ru : item.lead_en, label: (ru ? item.label_ru : item.label_en) || (ru ? "Новое" : "New"), details: ru ? item.details_ru : item.details_en, screenshot };
    });
    return { highlights, changes: release.changes, features: release.features, screenshots: release.screenshots.filter(shot => !used.has(shot.id)) };
  }
  const usedFeatures = new Set<number>();
  const usedChanges = new Set<number>();
  const usedScreenshots = new Set<string>();
  const highlights: Highlight[] = [];
  const plans = release.version === "0.3.1" ? plans031(ru) : [];

  for (const plan of plans) {
    const details: string[] = [];
    for (const index of plan.features ?? []) {
      if (release.features[index]) { details.push(release.features[index]); usedFeatures.add(index); }
    }
    for (const index of plan.changes ?? []) {
      if (release.changes[index]) { details.push(release.changes[index]); usedChanges.add(index); }
    }
    if (!details.length) continue;
    const screenshot = release.screenshots.find(shot => shot.sort_order === plan.shot);
    if (screenshot) usedScreenshots.add(screenshot.id);
    highlights.push({ ...plan, tone: releaseTones[highlights.length % releaseTones.length], details, screenshot });
  }

  release.features.forEach((text, index) => {
    if (usedFeatures.has(index)) return;
    const screenshot = matchingScreenshot(text, release.screenshots.filter(shot => !usedScreenshots.has(shot.id)));
    if (screenshot) usedScreenshots.add(screenshot.id);
    highlights.push({ id: `feature-${index}`, label: ru ? "Новая возможность" : "New feature", tone: releaseTones[highlights.length % releaseTones.length], ...splitNote(text), screenshot });
  });

  return {
    highlights,
    changes: release.changes.filter((_, index) => !usedChanges.has(index)),
    screenshots: release.screenshots.filter(shot => !usedScreenshots.has(shot.id)),
  };
}
