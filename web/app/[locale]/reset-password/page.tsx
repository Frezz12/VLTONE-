import type { Metadata } from "next";
import { PasswordForm } from "@/components/password-form";
import { Suspense } from "react";
export default async function Reset({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <Suspense><PasswordForm locale={locale} confirm /></Suspense>; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Новый пароль" : "Reset password", robots: { index: false, follow: true } };
}
