import type { Metadata } from "next";
import { AuthForm } from "@/components/auth-form";
export default async function Login({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <AuthForm locale={locale} mode="login" />; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Вход в аккаунт" : "Sign in", robots: { index: false, follow: true } };
}
