"use client";

import { useId, useState } from "react";
import { PortSymbol } from "./port-symbol";
import type { Locale, Localized, PortType } from "@/lib/creator/types";

type PortInfo = { id: PortType; name: string; color: string; description: Localized; example: string };
export function PortExplorer({ locale, types }: { locale: Locale; types: PortInfo[] }) {
  const [selected, setSelected] = useState(types[0].id);
  const active = types.find(type => type.id === selected)!;
  const id = useId();
  return <div className="creator-port-explorer" data-motion-scene>
    <div className="creator-type-options" role="group" aria-label={locale === "ru" ? "Типы портов" : "Port types"}>
      {types.map(type => <button type="button" key={type.id} aria-pressed={selected === type.id} aria-controls={id} onClick={() => setSelected(type.id)}><PortSymbol type={type.id} />{type.name}</button>)}
    </div>
    <div className="creator-port-explanation" id={id} style={{ borderColor: active.color }}>
      <div className="creator-connection-sample" aria-hidden="true" style={{ color: active.color }}><PortSymbol type={active.id} /><span className={active.id === "function" ? "callable-wire" : ""}><i className="loop-motion" /></span><PortSymbol type={active.id} /></div>
      <div aria-live="polite"><h3>{active.name}</h3><p>{active.description[locale]}</p><code>{active.example}</code></div>
    </div>
  </div>;
}
