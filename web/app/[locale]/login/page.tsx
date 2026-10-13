import type { Metadata } from "next";
import { AuthForm } from "@/components/auth-form";
export default async function Login({ params, searchParams }: { params: Promise<{ locale: string }>; searchParams: Promise<{ returnTo?: string }> }) { const { locale } = await params; const { returnTo } = await searchParams; return <AuthForm locale={locale} mode="login" returnTo={returnTo} />; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Вход в аккаунт" : "Sign in", robots: { index: false, follow: true } };
}
