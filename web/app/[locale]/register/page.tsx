import type { Metadata } from "next";
import { AuthForm } from "@/components/auth-form";
export default async function Register({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <AuthForm locale={locale} mode="register" />; }

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  return { title: locale === "ru" ? "Регистрация" : "Register", robots: { index: false, follow: true } };
}
