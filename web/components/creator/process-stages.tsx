"use client";

import { useState } from "react";
import { Check, ChevronRight } from "lucide-react";
import type { Locale } from "@/lib/creator/types";

export function ProcessStages({ locale, steps, label }: { locale: Locale; label: string; steps: { title: string; detail: string; code: string }[] }) {
  const [selected, setSelected] = useState(0);
  return <div className="creator-process-demo creator-dark-demo" data-motion-scene>
    <span className="creator-small-label">{label}</span>
    <div className="creator-process-tabs" role="group" aria-label={locale === "ru" ? "Этапы работы" : "Workflow stages"}>{steps.map((step, i) => <button key={step.title} type="button" aria-pressed={selected === i} onClick={() => setSelected(i)}><span>{i < selected ? <Check size={14} aria-hidden /> : String(i + 1).padStart(2, "0")}</span>{step.title}{i < steps.length - 1 && <ChevronRight size={14} aria-hidden />}</button>)}</div>
    <div className="creator-process-meter" aria-hidden><i className="loop-motion" style={{ width: `${(selected + 1) / steps.length * 100}%` }} /></div>
    <div className="creator-process-detail" aria-live="polite"><code>{steps[selected].code}</code><p>{steps[selected].detail}</p></div>
  </div>;
}
