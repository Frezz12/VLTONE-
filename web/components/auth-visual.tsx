"use client";

import { Pause, Play } from "lucide-react";
import { useEffect, useState } from "react";

export function AuthVisual({ locale }: { locale: string }) {
  const [paused, setPaused] = useState(false);
  const [hidden, setHidden] = useState(false);
  const [reduced, setReduced] = useState(false);
  const ru = locale === "ru";
  useEffect(() => {
    const motion = matchMedia("(prefers-reduced-motion: reduce)");
    const syncMotion = () => setReduced(motion.matches);
    const syncVisibility = () => setHidden(document.visibilityState !== "visible");
    syncMotion(); syncVisibility();
    motion.addEventListener("change", syncMotion); document.addEventListener("visibilitychange", syncVisibility);
    return () => { motion.removeEventListener("change", syncMotion); document.removeEventListener("visibilitychange", syncVisibility); };
  }, []);
  return <div className="auth-sound" data-paused={paused || hidden || reduced}>
    <div className="auth-spectrum" aria-hidden>{Array.from({ length: 48 }, (_, index) => <span key={index} style={{ height: `${12 + Math.sin(index * .43) ** 2 * (32 + Math.sin(index / 9) ** 2 * 48)}%`, animationDelay: `${-index * .12}s` }} />)}</div>
    <div className="auth-sound-foot"><span>{ru ? "Идея. Ритм. Твой звук." : "An idea. A rhythm. Your sound."}</span>{!reduced && <button type="button" aria-pressed={paused} onClick={() => setPaused(value => !value)} aria-label={paused ? (ru ? "Продолжить анимацию" : "Resume animation") : (ru ? "Остановить анимацию" : "Pause animation")}>{paused ? <Play size={16} aria-hidden /> : <Pause size={16} aria-hidden />}<span>{paused ? (ru ? "Продолжить" : "Resume") : (ru ? "Пауза" : "Pause")}</span></button>}</div>
  </div>;
}
