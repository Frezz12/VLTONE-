import type { Metadata } from "next";
import { ForumProvider, Shell } from "@/components/shell";
import "./globals.css";
export const metadata: Metadata = {
  title: { default: "VLTONE Forum", template: "%s — VLTONE Forum" },
  description: "Музыка, идеи и инструменты. Сообщество VLTONE.",
};
export default function Layout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="ru" suppressHydrationWarning>
      <body>
        <ForumProvider>
          <Shell>{children}</Shell>
        </ForumProvider>
      </body>
    </html>
  );
}
