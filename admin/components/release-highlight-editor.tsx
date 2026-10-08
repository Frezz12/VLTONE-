"use client";

import type { components } from "@vlt/api-client";
import { ArrowDown, ArrowUp, Eye, ImageIcon, Plus, Trash2, X } from "lucide-react";
import { useRef, useState } from "react";

export type ReleaseHighlight = components["schemas"]["ReleaseHighlight"];
type Screenshot = { id: string; caption_ru: string; caption_en: string; url: string; width: number; height: number };
export const highlightTones = [
  { value: "lime", label: "Салатовый", color: "#d5edaa" },
  { value: "coral", label: "Коралловый", color: "#efb3a6" },
  { value: "lilac", label: "Сиреневый", color: "#ccbef0" },
  { value: "sky", label: "Голубой", color: "#aecce9" },
  { value: "peach", label: "Персиковый", color: "#f0ceac" },
  { value: "mint", label: "Мятный", color: "#bce0cb" },
  { value: "rose", label: "Розовый", color: "#e9bbcf" },
  { value: "periwinkle", label: "Лавандовый", color: "#bfc9ed" },
] as const;
const paragraphs = (text: string) => text.split("\n").map(line => line.trim()).filter(Boolean);
const imageURL = (url: string) => url.startsWith("/v1/") ? `/api${url}` : url;

export function ReleaseHighlightEditor({ value, screenshots, onChange, disabled }: { value: ReleaseHighlight[]; screenshots: Screenshot[]; onChange: (value: ReleaseHighlight[]) => void; disabled: boolean }) {
  const [activeID, setActiveID] = useState<string>();
  const [locale, setLocale] = useState<"ru" | "en">("ru");
  const preview = useRef<HTMLDialogElement>(null);
  const active = value.find(item => item.id === activeID) ?? value[0];
  const index = value.findIndex(item => item.id === active?.id);
  function add() {
    const item: ReleaseHighlight = { id: crypto.randomUUID(), tone: highlightTones[value.length % highlightTones.length].value, title_ru: "", title_en: "", lead_ru: "", lead_en: "", label_ru: "Новое", label_en: "New", details_ru: [], details_en: [], screenshot_id: "" };
    onChange([...value, item]); setActiveID(item.id);
  }
  function edit(patch: Partial<ReleaseHighlight>) { if (active) onChange(value.map(item => item.id === active.id ? { ...item, ...patch } : item)); }
  function move(direction: number) { const next = [...value]; [next[index], next[index + direction]] = [next[index + direction], next[index]]; onChange(next); }

  return <section className="vlt-card vlt-card-pad" id="release-highlights" aria-label="Главные обновления">
    <div className="vlt-row vlt-between"><div><h2 className="vlt-section-title">Главные обновления</h2><p className="vlt-subtitle">Отдельные цветные блоки на сайте. Короткий текст, скриншот и подробности по нажатию.</p></div><button className="vlt-button vlt-button-secondary" onClick={() => preview.current?.showModal()} disabled={!value.length}><Eye size={16} aria-hidden />Предпросмотр</button></div>
    <div className="highlight-editor">
      <div className="highlight-block-list"><div role="group" aria-label="Блоки обновлений">{value.map((item, position) => <button type="button" key={item.id} className="highlight-block-button" aria-pressed={active?.id === item.id} onClick={() => setActiveID(item.id)}><span className="highlight-block-color" style={{ background: highlightTones.find(tone => tone.value === item.tone)?.color }} /><span><strong>{item.title_ru || `Новое обновление ${position + 1}`}</strong><small>{screenshots.find(shot => shot.id === item.screenshot_id)?.caption_ru || "Скриншот не выбран"}</small></span></button>)}</div><button className="vlt-button vlt-button-secondary" onClick={add} disabled={disabled || value.length >= 12}><Plus size={16} aria-hidden />Новый блок</button><small className="vlt-muted">{value.length} из 12 блоков</small></div>
      {active ? <div className="highlight-block-form"><div className="vlt-row vlt-between"><strong>Оформление блока</strong><div className="vlt-row"><button className="admin-icon-button" disabled={disabled || index === 0} onClick={() => move(-1)} aria-label="Переместить блок выше"><ArrowUp size={17} aria-hidden /></button><button className="admin-icon-button" disabled={disabled || index === value.length - 1} onClick={() => move(1)} aria-label="Переместить блок ниже"><ArrowDown size={17} aria-hidden /></button><button className="admin-icon-button" disabled={disabled} onClick={() => onChange(value.filter(item => item.id !== active.id))} aria-label="Удалить блок"><Trash2 size={17} aria-hidden /></button></div></div>
        <fieldset className="highlight-palette" disabled={disabled}><legend>Цвет блока</legend>{highlightTones.map(tone => <button key={tone.value} className="highlight-swatch" type="button" aria-label={tone.label} aria-pressed={active.tone === tone.value} onClick={() => edit({ tone: tone.value })}><span style={{ background: tone.color }} /></button>)}</fieldset>
        <div className="highlight-paired">{(["ru", "en"] as const).map(lang => <fieldset className="highlight-language" key={lang} disabled={disabled}><legend>{lang === "ru" ? "Русский" : "English"}</legend><label className="vlt-label">{lang === "ru" ? "Заголовок блока RU" : "Block title EN"}<input className="vlt-input" maxLength={160} value={active[`title_${lang}`]} onChange={event => edit({ [`title_${lang}`]: event.target.value })} /></label><label className="vlt-label">{lang === "ru" ? "Короткий текст RU" : "Short description EN"}<textarea className="vlt-input" maxLength={600} value={active[`lead_${lang}`]} onChange={event => edit({ [`lead_${lang}`]: event.target.value })} /></label><label className="vlt-label">{lang === "ru" ? "Метка RU" : "Label EN"}<input className="vlt-input" maxLength={60} value={active[`label_${lang}`]} onChange={event => edit({ [`label_${lang}`]: event.target.value })} /></label><label className="vlt-label">{lang === "ru" ? "Подробности RU — по одному пункту" : "Details EN — one item per line"}<textarea className="vlt-input" value={active[`details_${lang}`].join("\n")} onChange={event => edit({ [`details_${lang}`]: paragraphs(event.target.value) })} /></label></fieldset>)}</div>
        <label className="vlt-label">Скриншот блока<select className="vlt-input" value={active.screenshot_id} disabled={disabled} onChange={event => edit({ screenshot_id: event.target.value })}><option value="">Выберите изображение</option>{screenshots.map((shot, position) => <option value={shot.id} key={shot.id}>{shot.caption_ru || `Скриншот ${position + 1}`}</option>)}</select></label>{!screenshots.length && <p className="vlt-muted highlight-help"><ImageIcon size={16} aria-hidden />Сначала <a className="vlt-link" href="#release-screenshots">загрузите скриншоты</a> в этот релиз.</p>}
        {screenshots.find(shot => shot.id === active.screenshot_id) && <img className="highlight-image-preview" src={imageURL(screenshots.find(shot => shot.id === active.screenshot_id)!.url)} alt={active.title_ru || "Изображение блока"} />}
      </div> : <div className="highlight-empty"><ImageIcon size={32} strokeWidth={1.5} aria-hidden /><h3>Покажите, что изменилось</h3><p>Выберите самые заметные обновления. Остальные изменения и исправления можно оставить списком ниже.</p><button className="vlt-button" onClick={add} disabled={disabled}><Plus size={16} aria-hidden />Создать первый блок</button></div>}
    </div>
    <dialog className="highlight-preview-dialog" ref={preview} aria-label="Предпросмотр обновлений" onClick={event => { if (event.target === event.currentTarget) preview.current?.close(); }}><header><div><strong>Как это выглядит на сайте</strong><p>Несохранённые изменения тоже видны в предпросмотре.</p></div><div className="vlt-row"><select className="vlt-input" aria-label="Язык предпросмотра" value={locale} onChange={event => setLocale(event.target.value as "ru" | "en")}><option value="ru">Русский</option><option value="en">English</option></select><button className="admin-icon-button" aria-label="Закрыть предпросмотр" onClick={() => preview.current?.close()}><X size={20} aria-hidden /></button></div></header><div className="highlight-preview-content">{value.map(item => { const shot = screenshots.find(image => image.id === item.screenshot_id); return <article className="highlight-preview-card" key={item.id} style={{ background: highlightTones.find(tone => tone.value === item.tone)?.color }}><div><span>{item[`label_${locale}`] || (locale === "ru" ? "Новое" : "New")}</span><h3>{item[`title_${locale}`] || (locale === "ru" ? "Заголовок обновления" : "Update title")}</h3><p>{item[`lead_${locale}`] || (locale === "ru" ? "Кратко расскажите, что нового." : "Describe what’s new.")}</p>{item[`details_${locale}`].length > 0 && <details><summary>{locale === "ru" ? "Подробнее" : "Read more"}</summary><ul>{item[`details_${locale}`].map((text, index) => <li key={index}>{text}</li>)}</ul></details>}</div>{shot ? <img src={imageURL(shot.url)} alt={shot[`caption_${locale}`]} width={shot.width} height={shot.height} /> : <div className="highlight-preview-placeholder"><ImageIcon size={32} aria-hidden /><span>{locale === "ru" ? "Выберите скриншот" : "Choose a screenshot"}</span></div>}</article>; })}</div></dialog>
  </section>;
}
