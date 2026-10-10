"use client";

import { useState } from "react";
import { ArrowRight, RotateCcw } from "lucide-react";
import type { Locale } from "@/lib/creator/types";

export function MemoryDemo({ locale }: { locale: Locale }) {
  const [sample, setSample] = useState(0);
  const ru = locale === "ru";
  return <div className="creator-memory-demo creator-dark-demo" data-motion-scene>
    <div className="creator-lab-top"><span><i className="creator-status-dot" />History + Add</span><code>{ru ? "Сэмпл" : "Sample"} {sample}</code></div>
    <div className="creator-memory-loop">
      <div className="creator-memory-cell"><span>History</span><strong key={sample}>{(sample * .1).toFixed(1)}</strong><small>{ru ? "Предыдущее значение" : "Previous value"}</small></div>
      <div className="creator-memory-cable"><i className="loop-motion" /><ArrowRight size={21} aria-hidden /></div>
      <div className="creator-memory-cell"><span>Add</span><strong>+ 0.1</strong><small>Next: {((sample + 1) * .1).toFixed(1)}</small></div>
    </div>
    <div className="creator-feedback-wire" aria-hidden><span className="loop-motion" />← Next</div>
    <div className="creator-memory-actions"><button type="button" onClick={() => setSample(value => value + 1)}>{ru ? "Следующий сэмпл" : "Next sample"}<ArrowRight size={15} aria-hidden /></button><button type="button" aria-label={ru ? "Сбросить память" : "Reset memory"} onClick={() => setSample(0)}><RotateCcw size={16} aria-hidden /></button></div>
    <p>{ru ? "Нажми, чтобы пройти один шаг. History сначала отдаёт сохранённое значение, а затем записывает Next. Так обратная связь получает память." : "Click to advance one step. History returns its stored value first, then writes Next. This gives the feedback path memory."}</p>
  </div>;
}
