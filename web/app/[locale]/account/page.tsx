import type { Metadata } from "next";
import { AccountPanel } from "@/components/account-panel";
export default async function Account({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <AccountPanel locale={locale} />; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Ваш аккаунт" : "Your account", robots: { index: false, follow: true } };
}
