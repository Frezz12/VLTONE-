"use client";

import { useEffect, useId, useRef, useState, type CSSProperties } from "react";
import { PortSymbol } from "./port-symbol";
import type { Locale, PortType } from "@/lib/creator/types";

const nodes = [
  { id: "input", title: "Input", type: "audio", line: "Audio", ru: "Один сигнал идёт в две ветви: сухую и обработанную.", en: "One signal feeds two branches: dry and processed." },
  { id: "gain", title: "Gain", type: "audio", line: "Gain  ←  LFO", ru: "LFO управляет усилением и создаёт пульсацию громкости.", en: "The LFO controls gain to create a rhythmic volume pulse." },
  { id: "mix", title: "Mix", type: "audio", line: "A  +  B · Depth", ru: "Mix соединяет сухую ветвь A и обработанную B. Depth = 0 оставляет сухой звук.", en: "Mix blends dry branch A with processed branch B. Depth = 0 keeps the dry sound." },
  { id: "output", title: "Output", type: "audio", line: "Audio", ru: "Готовый сигнал возвращается в канал VLTone.", en: "The finished signal returns to the VLTone channel." },
  { id: "interface", title: "Interface", type: "number", line: "Rate   /   Depth", ru: "Две внешние ручки управляют скоростью LFO и глубиной эффекта.", en: "Two external controls set the LFO rate and effect depth." },
  { id: "lfo", title: "LFO", type: "number", line: "1.8 Hz · 0…1", ru: "Amount 0.5 и Offset 0.5 превращают синус в управляющий поток от 0 до 1.", en: "Amount 0.5 and Offset 0.5 turn the sine wave into a control stream from 0 to 1." },
] as const;
const edges = [
  ["input.out", "gain.in", "audio"], ["gain.out", "mix.b", "audio"], ["mix.out", "output.in", "audio"],
  ["input.out", "mix.a", "audio"], ["interface.rate", "lfo.rate", "number"],
  ["lfo.out", "gain.gain", "number"], ["interface.depth", "mix.mix", "number"],
] as const;
type Dock = "left" | "right" | "top" | "bottom";
type DemoPort = { id: string; type: PortType; side: Dock; at?: number; mobileSide?: Dock; mobileAt?: number };
const ports: Record<string, DemoPort[]> = {
  input: [{ id: "out", type: "audio", side: "right" }],
  gain: [{ id: "in", type: "audio", side: "left" }, { id: "out", type: "audio", side: "right" }, { id: "gain", type: "number", side: "bottom", at: 35 }],
  mix: [{ id: "a", type: "audio", side: "top", mobileSide: "left", mobileAt: 30 }, { id: "b", type: "audio", side: "left", mobileSide: "right" }, { id: "mix", type: "number", side: "bottom", at: 70, mobileSide: "top", mobileAt: 35 }, { id: "out", type: "audio", side: "right", mobileSide: "left", mobileAt: 70 }],
  output: [{ id: "in", type: "audio", side: "left", mobileSide: "right", mobileAt: 70 }],
  interface: [{ id: "rate", type: "number", side: "right" }, { id: "depth", type: "number", side: "top", at: 70, mobileSide: "bottom", mobileAt: 70 }],
  lfo: [{ id: "rate", type: "number", side: "left" }, { id: "out", type: "number", side: "top", at: 35 }],
};

export function TremoloDiagram({ locale }: { locale: Locale }) {
  const root = useRef<HTMLDivElement>(null);
  const [paths, setPaths] = useState<string[]>([]);
  const [size, setSize] = useState({ width: 1000, height: 350 });
  const [compact, setCompact] = useState(false);
  const [selected, setSelected] = useState("input");
  const [hovered, setHovered] = useState<string | null>(null);
  const [mode, setMode] = useState("all");
  const uid = useId().replace(/:/g, "");
  const active = hovered ?? selected;
  const detail = nodes.find(node => node.id === active)!;

  useEffect(() => {
    const el = root.current;
    if (!el) return;
    let frame = 0;
    const measure = () => {
      const bounds = el.getBoundingClientRect();
      const narrow = bounds.width <= 600;
      setCompact(narrow);
      setSize({ width: bounds.width, height: bounds.height });
      const position = (id: string) => {
        const port = el.querySelector<HTMLElement>(`[data-demo-port="${id}"]`)!;
        const rect = port.getBoundingClientRect();
        return { x: rect.left + rect.width / 2 - bounds.left, y: rect.top + rect.height / 2 - bounds.top, side: port.dataset.side as Dock };
      };
      const control = (p: ReturnType<typeof position>, length: number) => {
        const dx = p.side === "left" ? -length : p.side === "right" ? length : 0;
        const dy = p.side === "top" ? -length : p.side === "bottom" ? length : 0;
        return `${p.x + dx},${p.y + dy}`;
      };
      setPaths(edges.map(([from, to], index) => {
        const a = position(from), b = position(to);
        if (index === 3 && !narrow) {
          // The dry branch passes above the processing nodes.
          const corner = a.x + 14, track = 24;
          return `M${a.x},${a.y} Q${corner},${a.y} ${corner},${a.y - 14} V${track + 12} Q${corner},${track} ${corner + 12},${track} H${b.x - 12} Q${b.x},${track} ${b.x},${track + 12} V${b.y}`;
        }
        if (narrow && index === 1) {
          // The wet branch travels around the right edge, clear of the LFO.
          const track = bounds.width - 9;
          return `M${a.x},${a.y} H${track - 8} Q${track},${a.y} ${track},${a.y + 8} V${b.y - 8} Q${track},${b.y} ${track - 8},${b.y} H${b.x}`;
        }
        if (narrow && index === 3) {
          const track = (a.x + b.x) / 2;
          return `M${a.x},${a.y} C${track},${a.y} ${track},${a.y + 20} ${track},${a.y + 35} V${b.y - 20} Q${track},${b.y} ${b.x},${b.y}`;
        }
        const bend = a.side === "left" || a.side === "right" ? Math.max(12, Math.abs(b.x - a.x) / 2) : Math.max(26, Math.abs(b.y - a.y) / 2);
        return `M${a.x},${a.y} C${control(a, bend)} ${control(b, bend)} ${b.x},${b.y}`;
      }));
    };
    const schedule = () => { cancelAnimationFrame(frame); frame = requestAnimationFrame(measure); };
    const observer = new ResizeObserver(schedule);
    observer.observe(el);
    el.querySelectorAll("[data-node]").forEach(node => observer.observe(node));
    schedule();
    return () => { observer.disconnect(); cancelAnimationFrame(frame); };
  }, [compact]);

  return <div className="creator-demo" data-motion-scene>
    <div className="creator-demo-bar"><span><i /> Gentle Tremolo</span><div className="creator-segments" role="group" aria-label={locale === "ru" ? "Показать связи" : "Show connections"}>
      {[["all", locale === "ru" ? "Все связи" : "All wires"], ["audio", "Audio"], ["number", locale === "ru" ? "Модуляция" : "Modulation"]].map(([id, title]) => <button type="button" key={id} aria-pressed={mode === id} onClick={() => setMode(id)}>{title}</button>)}
    </div></div>
    <div className="creator-graph" ref={root} data-selected={active}>
      <svg className="creator-wires" viewBox={`0 0 ${size.width} ${size.height}`} aria-hidden="true">
        {paths.map((path, i) => { const [from, to, type] = edges[i]; const lit = (mode === "all" || mode === type) && (from.startsWith(`${active}.`) || to.startsWith(`${active}.`)); return <g key={i} data-wire-type={type} data-wire-active={lit} data-from={from} data-to={to} opacity={mode === "all" || mode === type ? 1 : .1}>
          <path id={`${uid}-wire-${i}`} d={path} className="creator-wire-base" />
          <path d={path} className="creator-wire-pulse loop-motion" style={{ animationDelay: `${i * -.3}s` }} />
        </g>; })}
      </svg>
      {nodes.map(node => <button type="button" key={node.id} className="creator-demo-node" data-node={node.id} data-type={node.type} aria-pressed={selected === node.id} aria-describedby={`${uid}-explanation`} onPointerEnter={event => { if (event.pointerType === "mouse") setHovered(node.id); }} onPointerLeave={() => setHovered(null)} onFocus={() => setHovered(node.id)} onBlur={() => setHovered(null)} onClick={() => { setSelected(node.id); setHovered(null); }}>
        {ports[node.id].map(port => <span className="creator-demo-port" key={port.id} data-demo-port={`${node.id}.${port.id}`} data-side={compact ? port.mobileSide ?? port.side : port.side} style={{ "--port-at": `${(compact ? port.mobileAt ?? port.at : port.at) ?? 50}%` } as CSSProperties} aria-hidden="true"><PortSymbol type={port.type} size={12} /></span>)}
        <span className="creator-node-title"><PortSymbol type={node.type as PortType} />{node.title}</span>
        <span className="creator-node-value">{node.line}</span>
        {node.id === "lfo" && <svg viewBox="0 0 120 20" className="creator-mini-wave" aria-hidden="true"><path d="M0 10C10-3 20-3 30 10S50 23 60 10 80-3 90 10 110 23 120 10" /></svg>}
      </button>)}
    </div>
    <div className="creator-demo-caption" id={`${uid}-explanation`} aria-live="polite"><strong>{detail.title}</strong><p>{detail[locale]}</p></div>
    <p className="creator-demo-hint">{locale === "ru" ? "Выбери ноду, чтобы проследить её связи. Схема объясняет обработку; звук здесь не воспроизводится." : "Select a node to trace its connections. This diagram explains processing; it does not play audio."}</p>
  </div>;
}
