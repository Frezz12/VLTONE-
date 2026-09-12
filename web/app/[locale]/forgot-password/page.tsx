import type { Metadata } from "next";
import { PasswordForm } from "@/components/password-form";
import { Suspense } from "react";
export default async function Forgot({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <Suspense><PasswordForm locale={locale} /></Suspense>; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Восстановление пароля" : "Password recovery", robots: { index: false, follow: true } };
}
