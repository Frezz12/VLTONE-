import type { Metadata } from "next";

export const siteUrl = "https://vltstudio.ru";

export function siteMetadata(locale: string, path: string, title: string, description: string): Metadata {
  const url = `${siteUrl}${path}`;
  return {
    title: { absolute: title }, description,
    alternates: { canonical: url },
    openGraph: { type: "website", siteName: "VLTone", title, description, url, locale: locale === "ru" ? "ru_RU" : "en_US", alternateLocale: locale === "ru" ? "en_US" : "ru_RU", images: [{ url: `${siteUrl}/opengraph-image`, width: 1200, height: 630, alt: title }] },
    twitter: { card: "summary_large_image", title, description, images: [`${siteUrl}/opengraph-image`] },
  };
}
