"use client";

import { useEffect, useRef } from "react";
import { screenshotVersion } from "@/lib/screenshot-version";

/** Decorative playback follows the page's existing motion and visibility controls. */
export function HeroVideo({ locale, poster }: { locale: string; poster: string }) {
  const ref = useRef<HTMLVideoElement>(null);

  useEffect(() => {
    const video = ref.current;
    const root = video?.closest<HTMLElement>(".studio-home");
    const scene = video?.closest<HTMLElement>("[data-motion-scene]");
    if (!video || !root || !scene) return;
    const reduced = window.matchMedia("(prefers-reduced-motion: reduce)");
    const update = () => {
      const paused = reduced.matches || document.hidden || root.dataset.motionPaused === "true"
        || scene.dataset.motionVisible !== "true";
      if (paused) video.pause();
      else {
        // Defer downloading the recording until motion is enabled and visible.
        const source = `/videos/studio-playback-${locale === "ru" ? "ru" : "en"}.mp4?v=${screenshotVersion}`;
        if (video.getAttribute("src") !== source) video.src = source;
        void video.play().catch(() => { /* The poster remains if autoplay is unavailable. */ });
      }
    };
    const observer = new MutationObserver(update);
    observer.observe(root, { attributes: true, attributeFilter: ["data-motion-paused"] });
    observer.observe(scene, { attributes: true, attributeFilter: ["data-motion-visible"] });
    reduced.addEventListener("change", update);
    document.addEventListener("visibilitychange", update);
    update();
    return () => {
      observer.disconnect();
      reduced.removeEventListener("change", update);
      document.removeEventListener("visibilitychange", update);
      video.pause();
    };
  }, [locale]);

  return <video ref={ref} poster={poster} width={2880} height={1800} muted loop playsInline
    preload="none" aria-hidden="true" tabIndex={-1} disablePictureInPicture />;
}
