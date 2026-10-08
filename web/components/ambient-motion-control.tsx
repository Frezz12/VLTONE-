"use client";

import { useEffect, useRef, useState } from "react";
import { Pause, Play } from "lucide-react";

export function AmbientMotionControl({ locale }: { locale: string }) {
  const button = useRef<HTMLButtonElement>(null);
  const [paused, setPaused] = useState(false);
  const [ready, setReady] = useState(false);
  const ru = locale === "ru";

  useEffect(() => {
    const root = button.current?.closest<HTMLElement>(".studio-home");
    if (!root) return;
    const scenes = root.querySelectorAll<HTMLElement>("[data-motion-scene]");
    const observer = new IntersectionObserver(entries => {
      entries.forEach(entry => {
        (entry.target as HTMLElement).dataset.motionVisible = String(entry.isIntersecting);
      });
    });
    scenes.forEach(scene => observer.observe(scene));
    const updateVisibility = () => { root.dataset.pageVisible = String(!document.hidden); };
    updateVisibility();
    document.addEventListener("visibilitychange", updateVisibility);
    setReady(true);
    return () => {
      observer.disconnect();
      document.removeEventListener("visibilitychange", updateVisibility);
      root.dataset.pageVisible = "false";
    };
  }, []);

  useEffect(() => {
    const root = button.current?.closest<HTMLElement>(".studio-home");
    if (root) root.dataset.motionPaused = String(paused);
  }, [paused]);

  const label = paused
    ? (ru ? "Продолжить анимацию" : "Resume animation")
    : (ru ? "Приостановить анимацию" : "Pause animation");
  return <button ref={button} type="button" className="ambient-motion-control" hidden={!ready} onClick={() => setPaused(value => !value)}>
    {paused ? <Play size={15} aria-hidden /> : <Pause size={15} aria-hidden />}<span>{label}</span>
  </button>;
}
