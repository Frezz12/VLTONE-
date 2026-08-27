import { AccountPanel } from "@/components/account-panel";
export default async function Account({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <AccountPanel locale={locale} />; }
