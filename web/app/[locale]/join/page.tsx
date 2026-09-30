import type { Metadata } from "next";
import { SessionJoinPage } from "@/components/session-join-page";

export default async function Join({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  return <SessionJoinPage locale={locale} />;
}

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return {
    title: locale === "ru" ? "Присоединиться к сессии" : "Join a session",
    robots: { index: false, follow: false },
    referrer: "no-referrer",
  };
}
