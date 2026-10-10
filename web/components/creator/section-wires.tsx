"use client";

import { useEffect, useRef, useState, type CSSProperties } from "react";

type Wire = { d: string; from: string; to: string; color: string };

export function SectionWires() {
  const svg = useRef<SVGSVGElement>(null);
  const [active, setActive] = useState<string | null>(null);
  const [geometry, setGeometry] = useState({ width: 1, height: 1, paths: [] as Wire[] });
  useEffect(() => {
    const parent = svg.current?.parentElement;
    if (!parent) return;
    let frame = 0;
    const sections = Array.from(parent.querySelectorAll<HTMLElement>(".creator-chapter"));
    const measure = () => {
      const point = (section: HTMLElement, side: string) => {
        const port = section.querySelector<HTMLElement>(`[data-section-port="${side}"]`)!;
        // Layout coordinates are stable while the reveal transform is animating.
        return { x: section.offsetLeft + port.offsetLeft, y: section.offsetTop + port.offsetTop };
      };
      const paths = sections.slice(1).map((section, i) => {
        const previous = sections[i], a = point(previous, "output"), b = point(section, "input");
        const middle = (a.y + b.y) / 2;
        return { d: `M${a.x},${a.y} C${a.x},${middle} ${b.x},${middle} ${b.x},${b.y}`, from: previous.id, to: section.id, color: previous.dataset.outputType || "audio" };
      });
      setGeometry({ width: parent.clientWidth, height: parent.scrollHeight, paths });
    };
    const schedule = () => { cancelAnimationFrame(frame); frame = requestAnimationFrame(measure); };
    const observer = new ResizeObserver(schedule);
    observer.observe(parent);
    sections.forEach(el => observer.observe(el));
    const highlight = (event: Event) => setActive((event.target as HTMLElement).closest?.(".creator-chapter")?.id ?? null);
    const clear = () => setActive(null);
    parent.addEventListener("pointerover", highlight);
    parent.addEventListener("focusin", highlight);
    parent.addEventListener("pointerleave", clear);
    schedule();
    return () => { observer.disconnect(); cancelAnimationFrame(frame); parent.removeEventListener("pointerover", highlight); parent.removeEventListener("focusin", highlight); parent.removeEventListener("pointerleave", clear); };
  }, []);
  useEffect(() => {
    const observer = new IntersectionObserver(entries => entries.forEach(entry => (entry.target as SVGElement).setAttribute("data-motion-visible", String(entry.isIntersecting))));
    svg.current?.querySelectorAll("g").forEach(group => observer.observe(group));
    return () => observer.disconnect();
  }, [geometry.paths.length]);
  return <svg ref={svg} className="creator-section-wires" viewBox={`0 0 ${geometry.width} ${geometry.height}`} aria-hidden="true">{geometry.paths.map((wire, i) => <g key={wire.from} data-motion-scene data-connected={active === wire.from || active === wire.to} style={{ "--wire-color": `var(--port-${wire.color})` } as CSSProperties}><path d={wire.d} className="creator-section-wire" /><path d={wire.d} className="creator-section-pulse loop-motion" style={{ animationDelay: `${i * -.2}s` }} /></g>)}</svg>;
}
