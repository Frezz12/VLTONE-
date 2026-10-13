import type { Metadata } from "next";
import { ForumLogin } from "@/components/forum-login";
export const metadata: Metadata = {
  title: "VLTONE Forum",
  robots: { index: false, follow: false },
};
export default async function Page({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  return <ForumLogin locale={(await params).locale} />;
}
