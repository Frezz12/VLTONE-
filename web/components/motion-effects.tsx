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

  useEffect(() => {
    const root = document.querySelector<HTMLElement>(".studio-home, .capabilities-main");
    if (!root) return;
    const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
    const animations = new Set<Animation>();
    let frame = 0;
    const cancel = () => { animations.forEach(animation => animation.cancel()); animations.clear(); };
    const pointer = () => { root.dataset.pointerInput = "true"; };
    const keyboard = () => { root.dataset.pointerInput = "false"; cancel(); };
    const disclose = (event: MouseEvent) => {
      const summary = (event.target as Element).closest(".faq-list summary, .capability-specs summary");
      if (!summary || event.detail === 0 || reduced.matches || root.dataset.motionPaused === "true") return;
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(() => {
        const details = summary.parentElement as HTMLDetailsElement;
        if (!details.open) return;
        const content = summary.nextElementSibling;
        if (!content) return;
        content.getAnimations().forEach(animation => animation.cancel());
        const animation = content.animate([{ opacity: 0, transform: "translateY(-4px)" }, { opacity: 1, transform: "none" }],
          { duration: 200, easing: "cubic-bezier(.23, 1, .32, 1)" });
        animations.add(animation);
        animation.onfinish = () => animations.delete(animation);
      });
    };
    root.addEventListener("pointerdown", pointer);
    root.addEventListener("keydown", keyboard);
    root.addEventListener("click", disclose);
    reduced.addEventListener("change", cancel);
    const pauseObserver = new MutationObserver(() => { if (root.dataset.motionPaused === "true") cancel(); });
    pauseObserver.observe(root, { attributes: true, attributeFilter: ["data-motion-paused"] });

    // The existing contents navigation follows the section being read.
    const links = Array.from(root.querySelectorAll<HTMLAnchorElement>(".capabilities-toc a"));
    const visible = new Set<Element>();
    const observer = "IntersectionObserver" in window ? new IntersectionObserver(entries => {
      entries.forEach(entry => { if (entry.isIntersecting) visible.add(entry.target); else visible.delete(entry.target); });
      const sections = Array.from(visible);
      const active = sections.find(section => `#${section.id}` === window.location.hash)
        ?? sections.sort((a, b) => a.getBoundingClientRect().top - b.getBoundingClientRect().top)[0];
      links.forEach(link => {
        if (active && link.hash === `#${active.id}`) link.setAttribute("aria-current", "location");
        else link.removeAttribute("aria-current");
      });
    }, { rootMargin: "-20% 0px -45% 0px" }) : undefined;
    root.querySelectorAll(".capability-detail").forEach(section => observer?.observe(section));
    return () => {
      cancelAnimationFrame(frame); cancel(); observer?.disconnect(); pauseObserver.disconnect();
      root.removeEventListener("pointerdown", pointer);
      root.removeEventListener("keydown", keyboard);
      root.removeEventListener("click", disclose);
      reduced.removeEventListener("change", cancel);
      links.forEach(link => link.removeAttribute("aria-current"));
    };
  }, [pathname]);

  return null;
}
