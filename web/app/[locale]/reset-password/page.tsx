import { PasswordForm } from "@/components/password-form";
import { Suspense } from "react";
export default async function Reset({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <Suspense><PasswordForm locale={locale} confirm /></Suspense>; }
