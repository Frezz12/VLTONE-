import type { Metadata } from "next";
import localFont from "next/font/local";
import "./globals.css";
const inter = localFont({ src: [
  { path: "../../fonts/Inter-Regular.ttf", weight: "400", style: "normal" },
  { path: "../../fonts/Inter-Medium.ttf", weight: "500", style: "normal" },
  { path: "../../fonts/Inter-SemiBold.ttf", weight: "600", style: "normal" },
  { path: "../../fonts/Inter-Bold.ttf", weight: "700", style: "normal" },
], display: "swap", variable: "--font-vltone" });
export const metadata: Metadata = { title: { default: "VLTone · Панель управления", template: "%s · VLTone" }, description: "Панель управления VLTone", robots: { index: false, follow: false } };
export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) { return <html lang="ru"><body className={inter.variable}>{children}</body></html>; }
