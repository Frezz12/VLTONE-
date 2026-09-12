import type { Metadata } from "next";

export const siteUrl = "https://vltstudio.ru";

export function siteMetadata(locale: string, path: string, title: string, description: string): Metadata {
  const url = `${siteUrl}/${locale}${path}`;
  return {
    title: { absolute: title }, description,
    alternates: { canonical: url, languages: { ru: `${siteUrl}/ru${path}`, en: `${siteUrl}/en${path}`, "x-default": `${siteUrl}/ru${path}` } },
    openGraph: { type: "website", siteName: "VLTone", title, description, url, locale: locale === "ru" ? "ru_RU" : "en_US", alternateLocale: locale === "ru" ? "en_US" : "ru_RU", images: [{ url: `${siteUrl}/${locale}/opengraph-image`, width: 1200, height: 630, alt: title }] },
    twitter: { card: "summary_large_image", title, description, images: [`${siteUrl}/${locale}/opengraph-image`] },
  };
}
