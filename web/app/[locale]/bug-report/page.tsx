import type { Metadata } from "next";
import { BugReportForm } from "@/components/bug-report-form";
export default async function BugReport({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <BugReportForm locale={locale} />; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Сообщить о баге" : "Report a bug", robots: { index: false, follow: true } };
}
