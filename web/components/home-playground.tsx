"use client";

import { useId, useState } from "react";
import Link from "next/link";
import { ArrowUpRight, AudioLines, GitBranch, MousePointer2, RotateCcw, SlidersHorizontal } from "lucide-react";
import { TremoloDiagram } from "@/components/creator/tremolo-diagram";
import { studioContent } from "@/lib/studio-content";
import "@/styles/home-playground.css";

function AutomationPreview({ ru }: { ru: boolean }) {
  const [shape, setShape] = useState(0);
  const [depth, setDepth] = useState(70);
  const id = useId();
  const modes = ru ? ["Подъём", "Пульс", "Приглушение"] : ["Rise", "Pulse", "Duck"];
  const descriptions = ru
    ? ["Плавно наращивай параметр к следующей части трека.", "Повторяй движение в ритме: каждый такт звучит по-новому.", "Освободи место для акцента и плавно верни уровень."]
    : ["Build a parameter gradually into the next part of the track.", "Repeat a movement in time and bring each bar to life.", "Make room for an accent, then gently bring the level back."];
  const points = Array.from({ length: 101 }, (_, i) => {
    const t = i / 100;
    const value = shape === 0 ? t : shape === 1 ? (1 - Math.cos(t * Math.PI * 8)) / 2 : 1 - Math.exp(-(t % .25) * 18);
    return `${(16 + t * 448).toFixed(1)},${(154 - value * depth * 1.2).toFixed(1)}`;
  });
  return <div className="home-demo automation-demo">
    <div className="home-demo-top"><span>{ru ? "Нарисуй движение" : "Draw a movement"}</span><span>4 {ru ? "такта" : "bars"}</span></div>
    <div className="automation-scope">
      <svg viewBox="0 0 480 190" role="img" aria-label={`${modes[shape]} · ${depth}%`}>
        <path className="home-demo-grid" d="M16 34H464M16 94H464M16 154H464M16 20V170M128 20V170M240 20V170M352 20V170M464 20V170" />
        <polygon className="automation-fill" points={`16,170 ${points.join(" ")} 464,170`} />
        <polyline className="automation-line" points={points.join(" ")} />
        {[0, 25, 50, 75, 100].map(index => <circle className="automation-point" key={index} cx={points[index].split(",")[0]} cy={points[index].split(",")[1]} r="4" />)}
        <g className="automation-cursor loop-motion" aria-hidden="true"><path d="M0 20V170" /><path d="M-4 18H4L0 24Z" fill="currentColor" /></g>
      </svg>
      <div className="automation-ruler" aria-hidden="true"><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span></div>
    </div>
    <div className="home-demo-modes" role="group" aria-label={ru ? "Форма автоматизации" : "Automation shape"}>
      {modes.map((mode, index) => <button type="button" key={mode} aria-pressed={shape === index} onClick={() => setShape(index)}>{mode}</button>)}
    </div>
    <label className="home-demo-range" htmlFor={`${id}-depth`}><span>{ru ? "Размах" : "Range"}<output>{depth}%</output></span><input id={`${id}-depth`} type="range" min="0" max="100" value={depth} onChange={event => setDepth(Number(event.target.value))} /></label>
    <p className="home-demo-note" aria-live="polite">{descriptions[shape]}</p>
  </div>;
}

function MixerPreview({ ru }: { ru: boolean }) {
  const [levels, setLevels] = useState([78, 62, 70]);
  const [muted, setMuted] = useState([false, false, false]);
  const id = useId();
  const names = ru ? ["Ударные", "Бас", "Клавиши"] : ["Drums", "Bass", "Keys"];
  const levelText = (level: number) => level === 0 ? "−∞ dB" : `${Math.round(20 * Math.log10(level / 100))} dB`;
  return <div className="home-demo mixer-demo">
    <div className="home-demo-top"><span>Night Bloom · {ru ? "баланс" : "balance"}</span><button type="button" className="home-demo-reset" aria-label={ru ? "Сбросить микшер" : "Reset mixer"} onClick={() => { setLevels([78, 62, 70]); setMuted([false, false, false]); }}><RotateCcw size={15} aria-hidden />{ru ? "Сброс" : "Reset"}</button></div>
    <div className="home-mixer-channels">
      {names.map((name, index) => <div className="home-mixer-channel" key={name} data-channel={index} data-muted={muted[index]}>
        <span className="home-channel-number" aria-hidden="true">0{index + 1}</span>
        <label htmlFor={`${id}-${index}`}>{name}</label>
        <div className="home-mixer-strip">
          <div className="home-mixer-meter" aria-hidden="true"><i style={{ transform: `scaleY(${muted[index] ? 0 : levels[index] / 100})` }}><b className="loop-motion" style={{ animationDelay: `${index * -.6}s` }} /></i></div>
          <input id={`${id}-${index}`} type="range" min="0" max="100" value={levels[index]} aria-valuetext={levelText(levels[index])} onChange={event => setLevels(current => current.map((level, i) => i === index ? Number(event.target.value) : level))} />
        </div>
        <output htmlFor={`${id}-${index}`}>{muted[index] ? (ru ? "Выкл." : "Muted") : levelText(levels[index])}</output>
        <button type="button" className="home-mixer-mute" aria-label={`${ru ? "Заглушить" : "Mute"}: ${name}`} aria-pressed={muted[index]} onClick={() => setMuted(current => current.map((value, i) => i === index ? !value : value))}>M</button>
      </div>)}
    </div>
    <p className="home-demo-note">{ru ? "Подвигай фейдеры. Нажми M, чтобы убрать дорожку из микса, и ещё раз — чтобы вернуть." : "Move the faders. Press M to take a track out of the mix, then press again to bring it back."}</p>
  </div>;
}

export function HomePlayground({ locale }: { locale: string }) {
  const ru = locale === "ru";
  return <section id="playground" className="home-playground section-space creator-theme" aria-labelledby="playground-title">
    <div className="studio-container">
      <header className="playground-heading" data-reveal="0">
        <div><span className="playground-eyebrow"><span className="playground-brand-glyph" aria-hidden="true"><i /><i /><i /><i /></span>VLTone / {ru ? "Звуковая мастерская" : "Sound workshop"}</span><h2 id="playground-title">{ru ? "А теперь —" : "Now, make it"}<br /><span>{ru ? "по-своему." : "your own."}</span></h2></div>
        <div className="playground-heading-note"><MousePointer2 size={25} strokeWidth={1.5} aria-hidden /><p>{ru ? "Нажми на ноду. Измени кривую. Подвигай фейдер. Здесь можно поэкспериментировать и понять, как всё устроено." : "Choose a node. Reshape a curve. Move a fader. This is your space to experiment and see how it all works."}</p><span>{ru ? "Три интерактивных примера · без звука" : "Three interactive examples · no audio"}</span></div>
      </header>

      <div className="playground-grid">
        <article className="playground-card playground-creator" aria-labelledby="home-creator-title" data-reveal="0" data-motion-scene>
          <div className="playground-card-copy">
            <span className="playground-eyebrow"><span className="playground-index">01</span><GitBranch size={16} aria-hidden />Creator</span>
            <h3 id="home-creator-title">{ru ? "Не нашёл свой эффект?" : "Missing the right effect?"}<br /><span>{ru ? "Собери его." : "Build it."}</span></h3>
            <p>{ru ? "Соединяй ноды в собственные эффекты и инструменты. Управляй звуком, добавляй модуляцию и выноси нужные параметры на ручки. Сохрани результат как мини-модуль и используй его в проекте." : "Connect nodes to make your own effects and instruments. Shape the signal, add modulation and expose the controls you need. Save the result as a mini-module and use it in your project."}</p>
            <Link className="text-link" href="/creator">{ru ? "Открыть Creator" : "Explore Creator"}<ArrowUpRight size={17} aria-hidden /></Link>
          </div>
          <div className="playground-creator-demo"><p className="playground-instruction">{ru ? "Выбери ноду — узнай, что она делает" : "Choose a node to see what it does"}<MousePointer2 size={14} aria-hidden /></p><TremoloDiagram locale={ru ? "ru" : "en"} /></div>
        </article>

        <article className="playground-card playground-automation" aria-labelledby="home-automation-title" data-reveal="0" data-motion-scene>
          <div className="playground-card-copy">
            <span className="playground-eyebrow"><span className="playground-index">02</span><AudioLines size={16} aria-hidden />{ru ? "Автоматизация" : "Automation"}</span>
            <h3 id="home-automation-title">{ru ? "Звук в движении." : "Sound in motion."}</h3>
            <p>{ru ? "Пусть громкость, панорама и параметры эффектов меняются вместе с треком. Расставляй точки и рисуй переходы: от мягкого вступления до ритмичной пульсации." : "Let volume, pan and effect parameters evolve with your track. Place points and shape transitions, from a gentle intro to a rhythmic pulse."}</p>
          </div>
          <AutomationPreview ru={ru} />
          <Link className="text-link" href="/manual#automation">{ru ? "Как работает автоматизация" : "How automation works"}<ArrowUpRight size={17} aria-hidden /></Link>
        </article>

        <article className="playground-card playground-mixer" aria-labelledby="home-mixer-title" data-reveal="60" data-motion-scene>
          <div className="playground-card-copy">
            <span className="playground-eyebrow"><span className="playground-index">03</span><SlidersHorizontal size={16} aria-hidden />{ru ? "Сведение" : "Mixing"}</span>
            <h3 id="home-mixer-title">{ru ? "Каждому звуку — место." : "A place for every sound."}</h3>
            <p>{ru ? "Выведи мелодию вперёд, поддержи её басом и оставь пространство ударным. Микшер собирает уровни, панораму, шины и посылы в одном месте. Когда всё звучит как нужно — экспортируй трек." : "Bring the melody forward, support it with bass and leave room for the drums. Levels, pan, buses and sends come together in the mixer. When it all fits, export your track."}</p>
          </div>
          <MixerPreview ru={ru} />
          <Link className="text-link" href="/capabilities#mixing">{ru ? "От баланса до готового трека" : "From balance to a finished track"}<ArrowUpRight size={17} aria-hidden /></Link>
        </article>
      </div>

      <nav className="playground-extras" aria-label={ru ? "Ещё возможности" : "More possibilities"}>{studioContent(locale).extras.map(item => <Link href={item.href} key={item.href}><strong>{item.title}<ArrowUpRight size={16} aria-hidden /></strong><p>{item.copy}</p></Link>)}</nav>
    </div>
  </section>;
}
