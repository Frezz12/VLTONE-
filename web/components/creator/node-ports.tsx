"use client";

import { useEffect, useId, useState } from "react";
import { PortSymbol } from "./port-symbol";
import type { DocumentedPort, Locale } from "@/lib/creator/types";

export function NodePorts({ name, inputs, outputs, locale, dynamic }: { name: string; inputs: DocumentedPort[]; outputs: DocumentedPort[]; locale: Locale; dynamic: boolean }) {
  const [selected, setSelected] = useState<string | null>(null);
  const [hover, setHover] = useState<string | null>(null);
  const id = useId();
  const active = hover ?? selected;
  const all = [...inputs.map(port => ({ ...port, key: `input-${port.id}`, direction: "input" })), ...outputs.map(port => ({ ...port, key: `output-${port.id}`, direction: "output" }))];
  const port = all.find(p => p.key === active);
  useEffect(() => {
    const rows = document.querySelectorAll<HTMLElement>("[data-port-row]");
    rows.forEach(row => { row.dataset.highlighted = String(row.dataset.portRow === active); });
    return () => rows.forEach(row => { delete row.dataset.highlighted; });
  }, [active]);
  return <div className="creator-port-diagram">
    <div className="creator-port-diagram-title"><span className="creator-status-dot" />{name}</div>
    <div className="creator-port-columns">{[inputs, outputs].map((ports, i) => <div key={i}>
      <span className="creator-small-label">{i === 0 ? (locale === "ru" ? "Входы" : "Inputs") : (locale === "ru" ? "Выходы" : "Outputs")}</span>
      {ports.map(p => { const key = `${i === 0 ? "input" : "output"}-${p.id}`; return <button type="button" key={p.id} aria-pressed={selected === key} aria-controls={id} data-lit={key === active} onPointerEnter={e => { if (e.pointerType === "mouse") setHover(key); }} onPointerLeave={() => setHover(null)} onFocus={() => setHover(key)} onBlur={() => setHover(null)} onClick={() => { setSelected(key); setHover(null); }}><PortSymbol type={p.type} /><span>{p.name}</span><small>{p.type}</small></button>; })}
      {!ports.length && <p className="creator-muted">{dynamic ? (locale === "ru" ? "Зависят от определения" : "Defined by the project") : "—"}</p>}
    </div>)}</div>
    <div className="creator-port-detail" id={id} aria-live="polite">{port ? <><strong>{port.name}</strong><span>{port.description}</span><code>{port.connection}</code></> : <span>{locale === "ru" ? "Выбери порт — увидишь его назначение и пример соединения. Строка в справочнике подсветится." : "Select a port to see its purpose and an example connection. Its reference row will highlight."}</span>}</div>
  </div>;
}
