"use client";

import { useEffect } from "react";
import { usePathname } from "next/navigation";

export function MotionEffects() {
  const pathname = usePathname();

  useEffect(() => {
    const elements = Array.from(document.querySelectorAll<HTMLElement>("[data-reveal]"));
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    if (!("IntersectionObserver" in window)) return;

    const observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        const element = entry.target as HTMLElement;
        element.animate(
          reducedMotion ? [
            { opacity: 0 },
            { opacity: 1 },
          ] : [
            { opacity: 0, transform: "translateY(22px) scale(.985)" },
            { opacity: 1, transform: "translateY(0) scale(1)" },
          ],
          {
            duration: reducedMotion ? 180 : 520,
            delay: reducedMotion ? 0 : Number(element.dataset.reveal || 0),
            easing: "cubic-bezier(.23, 1, .32, 1)",
            fill: "backwards",
          },
        );
        observer.unobserve(element);
      }
    }, { rootMargin: "0px 0px -8%", threshold: 0.08 });

    elements.forEach((element) => observer.observe(element));
    return () => observer.disconnect();
  }, [pathname]);

  return null;
}
