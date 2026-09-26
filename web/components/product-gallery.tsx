"use client";

import Image from "next/image";
import { Maximize2, X, ArrowUpRight } from "lucide-react";
import { useRef, useState } from "react";
import { screenshotVersion } from "@/lib/screenshot-version";

type Shot = { name: string; label: string; alt: string };

export function ProductShot({ locale, name, alt, eager = false }: { locale: string; name: string; alt: string; eager?: boolean }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const ru = locale === "ru";
  const src = `/images/studio/${name}-${ru ? "ru" : "en"}.webp?v=${screenshotVersion}`;
  return <>
    <button type="button" className="product-shot" onClick={() => dialog.current?.showModal()} aria-label={`${ru ? "Увеличить" : "Enlarge"}: ${alt}`}>
      <picture><source media="(max-width: 700px)" srcSet={src.replace(".webp", "-small.webp")} /><Image src={src} width={3200} height={2000} alt={alt} loading={eager ? "eager" : "lazy"} fetchPriority={eager ? "high" : "auto"} /></picture>
      <span className="shot-zoom" aria-hidden><Maximize2 size={16} /></span>
    </button>
    <dialog ref={dialog} className="product-lightbox" aria-label={alt} onClick={event => { if (event.target === event.currentTarget) dialog.current?.close(); }}>
      <div className="lightbox-toolbar"><p>{alt}</p><a href={src} target="_blank" rel="noopener noreferrer">{ru ? "Полный размер" : "Full size"}<ArrowUpRight size={16} aria-hidden /></a><button type="button" autoFocus onClick={() => dialog.current?.close()} aria-label={ru ? "Закрыть изображение" : "Close image"}><X size={22} aria-hidden /></button></div>
      <Image src={src} width={3200} height={2000} alt={alt} loading="lazy" />
    </dialog>
  </>;
}

export function StudioGallery({ locale, items }: { locale: string; items: Shot[] }) {
  const [selected, setSelected] = useState(0);
  const shot = items[selected];
  const ru = locale === "ru";
  return <div className="studio-gallery">
    <div className="studio-gallery-bar"><span className="gallery-label">{ru ? "Внутри VLTone" : "Inside VLTone"}</span><div className="gallery-switcher" role="group" aria-label={ru ? "Выбрать экран программы" : "Choose an app view"}>{items.map((item, index) => <button type="button" key={item.name} aria-pressed={index === selected} onClick={() => setSelected(index)}>{item.label}</button>)}</div><span className="gallery-counter">0{selected + 1} / 0{items.length}</span></div>
    <figure className="hero-product"><ProductShot locale={locale} name={shot.name} alt={shot.alt} eager /><figcaption aria-live="polite">{shot.alt}<span>{ru ? "Нажми, чтобы рассмотреть" : "Click to explore"}</span></figcaption></figure>
  </div>;
}
