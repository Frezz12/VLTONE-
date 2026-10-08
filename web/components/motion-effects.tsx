"use client";

import { useEffect } from "react";
import { usePathname } from "next/navigation";

export function MotionEffects() {
  const pathname = usePathname();

  useEffect(() => {
    const elements = Array.from(document.querySelectorAll<HTMLElement>("[data-reveal]"));
    const motionPreference = window.matchMedia("(prefers-reduced-motion: reduce)");
    const animations = new Set<Animation>();
    if (!("IntersectionObserver" in window)) return;

    const observer = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        const element = entry.target as HTMLElement;
        const requestedDelay = Number(element.dataset.reveal);
        const reducedMotion = motionPreference.matches;
        const animation = element.animate(
          reducedMotion ? [
            { opacity: 0 },
            { opacity: 1 },
          ] : [
            { opacity: 0, transform: "translateY(16px) scale(.992)" },
            { opacity: 1, transform: "translateY(0) scale(1)" },
          ],
          {
            duration: reducedMotion ? 160 : 420,
            delay: reducedMotion || !Number.isFinite(requestedDelay) ? 0 : requestedDelay,
            easing: "cubic-bezier(.23, 1, .32, 1)",
            fill: "backwards",
          },
        );
        animations.add(animation);
        animation.onfinish = () => animations.delete(animation);
        observer.unobserve(element);
      }
    }, { rootMargin: "0px 0px -8%", threshold: 0.08 });

    elements.forEach((element) => observer.observe(element));
    const cancelMotion = () => {
      animations.forEach(animation => animation.cancel());
      animations.clear();
    };
    motionPreference.addEventListener("change", cancelMotion);
    return () => { observer.disconnect(); cancelMotion(); motionPreference.removeEventListener("change", cancelMotion); };
  }, [pathname]);

  return null;
}
