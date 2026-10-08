"use client";

import Image from "next/image";
import { Maximize2, X, ArrowUpRight } from "lucide-react";
import { useRef, useState } from "react";
import { screenshotVersion } from "@/lib/screenshot-version";

type Shot = { name: string; label: string; alt: string };

function GalleryVisual({ view }: { view: number }) {
  return <svg className="gallery-visual" viewBox="0 0 160 64" fill="none" aria-hidden="true">
    {view === 0 ? <>
      {[8, 28, 48].map((y, index) => <g key={y}>
        <rect x="7" y={y} width="146" height="14" rx="2" fill="currentColor" opacity=".12" />
        {index === 0 ? Array.from({ length: 25 }, (_, beat) => {
          const height = 3 + Math.sin(beat * .8) ** 2 * 9;
          return <rect key={beat} className="gallery-wave loop-motion" x={13 + beat * 5.6} y={y + 7 - height / 2} width="2" height={height} rx="1" fill="currentColor" style={{ animationDelay: (-beat * .12) + "s" }} />;
        }) : [12, 52, 98].map((x, clip) => <rect key={x} x={x} y={y + 3} width={clip === 1 ? 34 : 42} height="8" rx="1" fill="currentColor" opacity={index === 1 ? .75 : .45} />)}
      </g>)}
    </> : view === 1 ? <>
      {[16, 32, 48].map(y => <path key={y} d={`M6 ${y}H154`} stroke="currentColor" opacity=".15" />)}
      {[[10, 44, 30], [25, 27, 40], [49, 10, 25], [64, 36, 28], [87, 19, 36], [111, 44, 25], [133, 27, 20]].map(([x, y, width], index) => <rect key={x} className="gallery-note loop-motion" x={x} y={y} width={width} height="7" rx="2" fill="currentColor" style={{ animationDelay: (-index * .4) + "s" }} />)}
    </> : view === 2 ? <>
      {[18, 43, 68, 93, 118, 143].map((x, index) => <g key={x}>
        <path d={`M${x} 5V59`} stroke="currentColor" opacity=".2" strokeWidth="3" />
        <rect className="gallery-level loop-motion" x={x - 5} y={18 + (index % 3) * 8} width="10" height={36 - (index % 3) * 8} rx="2" fill="currentColor" opacity=".35" style={{ animationDelay: (-index * .25) + "s" }} />
        <rect className="gallery-fader" x={x - 8} y={12 + (index % 4) * 9} width="16" height="6" rx="2" fill="currentColor" />
      </g>)}
    </> : <>
      {[16, 32, 48].map(y => <path key={y} d={`M6 ${y}H154`} stroke="currentColor" opacity=".12" />)}
      <path d="M7 56C20 56 24 18 40 26S60 45 75 36 93 13 110 23 129 32 153 20" stroke="currentColor" strokeWidth="2.5" />
      {[[40, 26], [75, 36], [110, 23]].map(([x, y]) => <circle key={x} cx={x} cy={y} r="4" fill="currentColor" />)}
    </>}
  </svg>;
}

export function ProductShot({ locale, name, alt, eager = false, onReady, source }: { locale: string; name: string; alt: string; eager?: boolean; onReady?: () => void; source?: { src: string; width: number; height: number } }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const ru = locale === "ru";
  const src = source?.src ?? `/images/studio/${name}-${ru ? "ru" : "en"}.webp?v=${screenshotVersion}`;
  return <>
    <button type="button" className="product-shot" data-shot={name} onClick={() => dialog.current?.showModal()} aria-label={`${ru ? "Увеличить" : "Enlarge"}: ${alt}`}>
      <picture>{!source && !name.startsWith("showcase-") && <source media="(max-width: 700px)" srcSet={src.replace(".webp", "-small.webp")} />}<Image src={src} width={source?.width ?? 2880} height={source?.height ?? 1800} alt={alt} loading={eager ? "eager" : "lazy"} onLoad={onReady} /></picture>
      <span className="shot-zoom" aria-hidden><Maximize2 size={16} /></span>
    </button>
    <dialog ref={dialog} className="product-lightbox" aria-label={alt} onClick={event => { if (event.target === event.currentTarget) dialog.current?.close(); }}>
      <div className="lightbox-toolbar"><p>{alt}</p><a href={src} target="_blank" rel="noopener noreferrer">{ru ? "Полный размер" : "Full size"}<ArrowUpRight size={16} aria-hidden /></a><button type="button" autoFocus onClick={() => dialog.current?.close()} aria-label={ru ? "Закрыть изображение" : "Close image"}><X size={22} aria-hidden /></button></div>
      <Image src={src} width={source?.width ?? 3200} height={source?.height ?? 2000} alt={alt} loading="lazy" />
    </dialog>
  </>;
}

export function StudioGallery({ locale, items }: { locale: string; items: Shot[] }) {
  const [selected, setSelected] = useState(0);
  const [pointerMotion, setPointerMotion] = useState(false);
  const preview = useRef<HTMLElement>(null);
  const animateNext = useRef(false);
  const direction = useRef(1);
  const warmed = useRef(new Set<string>());
  const shot = items[selected];
  const ru = locale === "ru";
  const shortLabels = ru ? ["Проект", "Ноты", "Микшер", "EQ"] : ["Arrange", "Notes", "Mixer", "EQ"];
  function warmShot(index: number) {
    const src = `/images/studio/${items[index].name}-${ru ? "ru" : "en"}.webp?v=${screenshotVersion}`;
    if (warmed.current.has(src)) return;
    warmed.current.add(src);
    const image = new window.Image();
    image.src = src;
  }
  function selectShot(index: number, withMotion: boolean) {
    animateNext.current = withMotion && index !== selected;
    direction.current = index > selected ? 1 : -1;
    preview.current?.querySelector("picture")?.getAnimations().forEach(animation => animation.cancel());
    setPointerMotion(withMotion);
    setSelected(index);
  }
  function revealShot() {
    if (!animateNext.current) return;
    animateNext.current = false;
    if (window.matchMedia("(prefers-reduced-motion: reduce)").matches) return;
    const picture = preview.current?.querySelector("picture");
    const easing = preview.current ? getComputedStyle(preview.current).getPropertyValue("--vlt-ease-out").trim() : "";
    picture?.getAnimations().forEach(animation => animation.cancel());
    picture?.animate([{ opacity: .35, transform: `translateX(${direction.current * 14}px) scale(.992)` }, { opacity: 1, transform: "none" }], { duration: 240, easing: easing || "cubic-bezier(.23, 1, .32, 1)" });
  }
  return <div className="studio-gallery" data-motion-scene>
    <div className="studio-gallery-bar" data-pointer-motion={pointerMotion}>
      <div className="gallery-switcher" role="group" aria-label={ru ? "Выбрать экран программы" : "Choose an app view"}>
        {items.map((item, index) => <button type="button" className="gallery-channel" data-view={index} key={item.name} aria-label={item.label} aria-pressed={index === selected} onPointerEnter={() => warmShot(index)} onFocus={() => warmShot(index)} onClick={event => selectShot(index, event.detail > 0)}>
          <GalleryVisual view={index} />
          <span className="gallery-channel-label"><span className="gallery-label-full">{item.label}</span><span className="gallery-label-short" aria-hidden>{shortLabels[index]}</span><ArrowUpRight size={14} aria-hidden /></span>
        </button>)}
      </div>
      <div className="gallery-scrubber">
        <input type="range" min="0" max={items.length - 1} step="1" value={selected} aria-label={ru ? "Переключить экран программы" : "Switch app view"} aria-valuetext={shot.label} onPointerDown={() => items.forEach((_, index) => warmShot(index))} onChange={event => selectShot(Number(event.target.value), false)} />
      </div>
    </div>
    <figure ref={preview} className="hero-product"><ProductShot locale={locale} name={shot.name} alt={shot.alt} onReady={revealShot} /><figcaption aria-live="polite">{shot.alt}<span>{ru ? "Перетаскивай маркер или выбери экран" : "Drag the marker or choose a view"}</span></figcaption></figure>
  </div>;
}
