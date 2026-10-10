import type { CSSProperties } from "react";
import type { PortType } from "@/lib/creator/types";

export function PortSymbol({ type, size = 18 }: { type: PortType; size?: number }) {
  return <svg className="creator-port-symbol" style={{ "--port-color": `var(--port-${type})` } as CSSProperties} viewBox="0 0 20 20" width={size} height={size} aria-hidden="true">
    {type === "audio" ? <circle cx="10" cy="10" r="5.5" />
      : type === "number" ? <path d="m10 3 7 7-7 7-7-7Z" />
      : type === "gate" ? <rect x="4" y="4" width="12" height="12" rx="1" />
      : type === "function" ? <path d="m10 2 6 4v8l-6 4-6-4V6Z" />
      : type === "integer" ? <path d="m10 3 7 13H3Z" />
      : type === "array" ? <><rect x="3" y="4" width="14" height="12" /><path className="port-cut" d="M8 4v12" /></>
      : type === "list" ? <path fill="none" stroke="currentColor" strokeWidth="2" d="M3 5h14M3 10h14M3 15h14" />
      : <><rect x="2" y="5" width="16" height="10" rx="5" /><path className="port-cut" d="M11 5v10" /></>}
  </svg>;
}
