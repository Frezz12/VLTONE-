"use client";

import Link from "next/link";
import { ArrowUpRight, Search, Users, X } from "lucide-react";
import { useEffect, useRef, useState, type RefObject } from "react";
import { adminNavigation } from "./admin-navigation";

export function AdminCommandMenu({ dialog }: { dialog: RefObject<HTMLDialogElement | null> }) {
  const [query, setQuery] = useState("");
  const [selected, setSelected] = useState(0);
  const links = useRef<Array<HTMLAnchorElement | null>>([]);
  const search = query.trim().toLowerCase();
  const destinations = adminNavigation.flatMap(group => group.links).filter(item => `${item.label} ${item.keywords}`.toLowerCase().includes(search));
  const options = query.trim().length >= 2 ? [...destinations, { href: `/users?q=${encodeURIComponent(query.trim())}`, label: `Найти пользователя: ${query.trim()}`, icon: Users, keywords: "" }] : destinations;

  useEffect(() => { setSelected(0); }, [query]);
  useEffect(() => { if (dialog.current?.open) links.current[selected]?.scrollIntoView({ block: "nearest" }); }, [selected, dialog]);

  return <dialog ref={dialog} className="admin-command-menu" aria-label="Быстрый переход" onClose={() => { setQuery(""); setSelected(0); }} onClick={event => { if (event.target === event.currentTarget) dialog.current?.close(); }}>
    <div className="admin-command-input"><Search size={20} aria-hidden /><input autoFocus value={query} onChange={event => setQuery(event.target.value)} placeholder="Раздел, имя или email…" aria-label="Найти раздел или пользователя" role="combobox" aria-expanded="true" aria-controls="admin-command-options" aria-autocomplete="list" aria-activedescendant={options[selected] ? `admin-command-${selected}` : undefined} onKeyDown={event => {
      if (event.key === "ArrowDown" || event.key === "ArrowUp") { event.preventDefault(); setSelected(value => options.length ? (value + (event.key === "ArrowDown" ? 1 : options.length - 1)) % options.length : 0); }
      if (event.key === "Enter" && options[selected]) { event.preventDefault(); links.current[selected]?.click(); }
    }} /><button type="button" className="admin-icon-button" aria-label="Закрыть быстрый переход" onClick={() => dialog.current?.close()}><X size={18} aria-hidden /></button></div>
    <div id="admin-command-options" className="admin-command-options" role="listbox" aria-label="Результаты поиска">
      {options.map((option, index) => <Link key={option.href} id={`admin-command-${index}`} ref={element => { links.current[index] = element; }} href={option.href} role="option" aria-selected={selected === index} onFocus={() => setSelected(index)} onPointerMove={() => setSelected(index)} onClick={() => dialog.current?.close()}><option.icon size={19} aria-hidden /><span>{option.label}</span><ArrowUpRight size={15} aria-hidden /></Link>)}
      {!options.length && <p className="vlt-muted">Раздел не найден.</p>}
    </div>
    <footer><span>↑ ↓ выбор</span><span>Enter открыть</span><span>Esc закрыть</span></footer>
  </dialog>;
}
