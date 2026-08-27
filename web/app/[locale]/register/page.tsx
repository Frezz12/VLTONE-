import { AuthForm } from "@/components/auth-form";
export default async function Register({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <AuthForm locale={locale} mode="register" />; }
