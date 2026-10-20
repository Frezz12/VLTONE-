"use client";

import { useEffect, useRef } from "react";
import { usePathname } from "next/navigation";

export function SiteVisitTracker() {
  const pathname = usePathname();
  const previous = useRef<string | null>(null);
  useEffect(() => {
    if (!pathname || previous.current === pathname || navigator.doNotTrack === "1" || (navigator as Navigator & { globalPrivacyControl?: boolean }).globalPrivacyControl) return;
    // Track actual visible pages, never prefetched routes or hidden prerenders.
    const track = () => {
      if (document.visibilityState !== "visible" || previous.current === pathname) return;
      const referrer = previous.current ? window.location.origin : document.referrer;
      previous.current = pathname;
      let origin = "";
      try { origin = referrer ? new URL(referrer).origin : ""; } catch { /* Invalid/absent referrer is direct traffic. */ }
      void fetch("/api/v1/site-visits", {
        method: "POST", credentials: "same-origin", keepalive: true,
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ id: crypto.randomUUID(), path: window.location.pathname, referrer: origin }),
      }).catch(() => { /* Analytics must never interrupt navigation. */ });
    };
    track();
    document.addEventListener("visibilitychange", track);
    return () => document.removeEventListener("visibilitychange", track);
  }, [pathname]);
  return null;
}
