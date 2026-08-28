import type { Metadata } from "next";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { ManualView } from "@/components/manual-view";

export async function generateMetadata({ params }: { params: Promise<{ locale: "ru" | "en" }> }): Promise<Metadata> {
  const { locale } = await params;
  const t = await getTranslations({ locale, namespace: "Manual" });
  return { title: t("metaTitle"), description: t("metaDescription") };
}

export default async function ManualPage({ params }: { params: Promise<{ locale: "ru" | "en" }> }) {
  const { locale } = await params;
  setRequestLocale(locale);
  return <ManualView locale={locale} />;
}
