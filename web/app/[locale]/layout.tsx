import type { Metadata } from "next";
import { NextIntlClientProvider } from "next-intl";
import { getMessages, setRequestLocale } from "next-intl/server";
import { notFound } from "next/navigation";
import { locales } from "@/i18n/request";
import { Header } from "@/components/header";
import { Footer } from "@/components/footer";
import localFont from "next/font/local";
import { siteUrl } from "@/lib/seo";
import "../globals.css";

const inter = localFont({ src: [
  { path: "../../fonts/Inter-Light.ttf", weight: "350", style: "normal" },
  { path: "../../fonts/Inter-Regular.ttf", weight: "400", style: "normal" },
], display: "swap", variable: "--font-vltone" });

export const metadata: Metadata = { metadataBase: new URL(siteUrl), title: { default: "VLTone", template: "%s — VLTone" }, applicationName: "VLTone", icons: { icon: "/favicon.svg", apple: "/apple-icon.png" } };
export function generateStaticParams() { return locales.map((locale) => ({ locale })); }

export default async function LocaleLayout({ children, params }: Readonly<{ children: React.ReactNode; params: Promise<{ locale: string }> }>) {
  const { locale } = await params;
  if (!locales.includes(locale as (typeof locales)[number])) notFound();
  setRequestLocale(locale);
  const messages = await getMessages();
  return (
    <html lang={locale}>
      <body className={inter.variable}>
        <NextIntlClientProvider messages={messages}>
          <div className="vlt-shell"><Header locale={locale} />{children}<Footer locale={locale} /></div>
        </NextIntlClientProvider>
      </body>
    </html>
  );
}
