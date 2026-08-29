"use client";

import { api } from "@vlt/api-client";
import { Activity, BellRing, Bot, Bug, CircleGauge, Files, MessageSquareText, PackageOpen, ShieldCheck, Users, X } from "lucide-react";
import Link from "next/link";
import { usePathname } from "next/navigation";
import { useEffect, useState } from "react";
import { adminPollingAllowed, markAdminActivity } from "./admin-activity";

const links = [
  ["/", "Обзор", CircleGauge], ["/users", "Пользователи", Users], ["/bugs", "Баги", Bug],
  ["/crashes", "Краши", Activity], ["/models", "Модели AI", Bot], ["/prompts", "Промпты", MessageSquareText],
  ["/releases", "Релизы", PackageOpen],
  ["/audit", "Аудит", Files],
] as const;

type CrashSummary = { id: string; app_version: string; platform: string; reason: string; occurred_at: string };
const lastCrashKey = "vlt-admin-last-crash";

export function AdminShell({ children }: { children: React.ReactNode }) {
  const pathname = usePathname();
  const [latestCrash, setLatestCrash] = useState<CrashSummary>();
  const [notificationPermission, setNotificationPermission] = useState<NotificationPermission>("default");

  useEffect(() => {
    if ("Notification" in window) setNotificationPermission(Notification.permission);
    markAdminActivity();
    const markActive = () => markAdminActivity();
    window.addEventListener("pointerdown", markActive, { passive: true });
    window.addEventListener("keydown", markActive);
    let cancelled = false;
    async function pollCrashes() {
      if (!adminPollingAllowed()) return;
      try {
        const result = await api.request<{ crashes: CrashSummary[] }>("/v1/admin/crashes?limit=1");
        if (cancelled || !result.crashes[0]) return;
        const crash = result.crashes[0];
        const previous = window.localStorage.getItem(lastCrashKey);
        window.localStorage.setItem(lastCrashKey, crash.id);
        if (!previous || previous === crash.id) return;
        setLatestCrash(crash);
        if ("Notification" in window && Notification.permission === "granted") {
          new Notification("VLT Studio Pro: новый краш", {
            body: `${crash.app_version} · ${crash.platform} · ${crash.reason}`,
            tag: `vlt-crash-${crash.id}`,
          });
        }
      } catch {
        // Authentication redirects and transient network failures are handled
        // by the page itself; the monitor simply tries again on the next tick.
      }
    }
    void pollCrashes();
    const timer = window.setInterval(() => void pollCrashes(), 15000);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
      window.removeEventListener("pointerdown", markActive);
      window.removeEventListener("keydown", markActive);
    };
  }, []);

  async function enableNotifications() {
    if (!("Notification" in window)) return;
    try {
      setNotificationPermission(await Notification.requestPermission());
    } catch {
      setNotificationPermission("denied");
    }
  }

  return <div className="admin-shell">
    <a className="admin-skip-link" href="#admin-main">К основному содержимому</a>
    <aside className="admin-side">
      <Link className="vlt-brand" href="/"><span className="vlt-brand-mark">VLT</span><span>Control</span></Link>
      <nav className="admin-nav" aria-label="Администрирование">{links.map(([href, label, Icon]) => {
        const active = href === "/" ? pathname === "/" : pathname.startsWith(href);
        return <Link href={href} className={active ? "active" : undefined} aria-current={active ? "page" : undefined} key={href}><Icon size={16} aria-hidden />{label}</Link>;
      })}</nav>
      <div className="admin-side-foot">
        <button className="admin-notification-button" onClick={() => void enableNotifications()} disabled={notificationPermission === "granted" || notificationPermission === "denied"}><BellRing size={16} aria-hidden />{notificationPermission === "granted" ? "Уведомления включены" : notificationPermission === "denied" ? "Уведомления запрещены" : "Включить уведомления"}</button>
        <div className="vlt-muted"><ShieldCheck size={16} aria-hidden /> Защищённая зона</div>
      </div>
    </aside>
    <main className="admin-main" id="admin-main" tabIndex={-1}>
      {latestCrash && <div className="admin-crash-alert" role="status"><BellRing size={18} aria-hidden /><Link href="/crashes"><strong>Новый краш VLT Studio Pro</strong><span>{latestCrash.app_version} · {latestCrash.platform} · {latestCrash.reason}</span></Link><button onClick={() => setLatestCrash(undefined)} aria-label="Закрыть уведомление"><X size={16} /></button></div>}
      {children}
    </main>
  </div>;
}
