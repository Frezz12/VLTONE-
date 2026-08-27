import { BugReportForm } from "@/components/bug-report-form";
export default async function BugReport({ params }: { params: Promise<{ locale: string }> }) { const { locale } = await params; return <BugReportForm locale={locale} />; }
