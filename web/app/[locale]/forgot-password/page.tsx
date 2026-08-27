import { PasswordForm } from "@/components/password-form";
import { Suspense } from "react";
export default async function Forgot({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <Suspense><PasswordForm locale={locale} /></Suspense>; }
