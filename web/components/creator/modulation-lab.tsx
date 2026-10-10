"use client";

import { useId, useState } from "react";
import type { Locale } from "@/lib/creator/types";

export function ModulationLab({ locale }: { locale: Locale }) {
  const [rate, setRate] = useState(1.8);
  const [depth, setDepth] = useState(.35);
  const id = useId();
  const ru = locale === "ru";
  const wave = Array.from({ length: 241 }, (_, i) => {
    const x = i * 2.5;
    const y = 90 - Math.sin(i / 240 * Math.PI * 2 * rate * 2) * depth * 68;
    return `${i ? "L" : "M"}${x.toFixed(1)},${y.toFixed(1)}`;
  }).join(" ");
  return <div className="creator-modulation-lab creator-dark-demo" data-motion-scene>
    <div className="creator-lab-top"><span><i className="creator-status-dot" />Gentle Tremolo</span><code>LFO → Gain → Mix</code></div>
    <svg className="creator-live-wave" viewBox="0 0 600 180" role="img" aria-label={ru ? `Модель модуляции: ${rate} Hz, глубина ${Math.round(depth * 100)}%` : `Modulation model: ${rate} Hz, depth ${Math.round(depth * 100)}%`}>
      <path className="creator-wave-grid" d="M0 45H600M0 90H600M0 135H600M100 0V180M200 0V180M300 0V180M400 0V180M500 0V180" />
      <path className="creator-wave-ghost" d="M0 90H600" />
      <path className="creator-wave-line" d={wave} />
      <g className="creator-scope-playhead loop-motion" style={{ animationDuration: `${6 / rate}s` }}><path d="M0 16V164" /><circle cy="90" r="4" /></g>
    </svg>
    <div className="creator-lab-controls"><label htmlFor={`${id}-rate`}><span>Rate<output>{rate.toFixed(1)} Hz</output></span><input id={`${id}-rate`} type="range" min="0.1" max="10" step="0.1" value={rate} onChange={event => setRate(Number(event.target.value))} /></label><label htmlFor={`${id}-depth`}><span>Depth<output>{Math.round(depth * 100)}%</output></span><input id={`${id}-depth`} type="range" min="0" max="1" step="0.01" value={depth} onChange={event => setDepth(Number(event.target.value))} /></label></div>
    <p>{ru ? "Подвигай ручки: Rate меняет скорость, Depth — размах модуляции. Это визуальная модель без воспроизведения звука." : "Move the controls: Rate changes the speed, Depth changes the modulation range. This is a visual model without audio playback."}</p>
  </div>;
}
