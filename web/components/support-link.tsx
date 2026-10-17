"use client";

import type { ReactNode } from "react";

export function SupportLink({ source, className, children }: { source: "home" | "footer"; className?: string; children: ReactNode }) {
  function recordClick() {
    // Navigation stays native and immediate even when statistics are unavailable.
    void fetch(`/api/v1/support/boosty/click/${source}`, { method: "POST", keepalive: true }).catch(() => {});
  }
  return <a href="https://boosty.to/vltone" target="_blank" rel="noopener noreferrer" className={className}
    onClick={recordClick} onAuxClick={event => { if (event.button === 1) recordClick(); }}>{children}</a>;
}
