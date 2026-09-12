import type { MetadataRoute } from "next";
import { siteUrl } from "@/lib/seo";

export default function sitemap(): MetadataRoute.Sitemap {
  return ["", "/manual", "/releases"].flatMap((path) => ["ru", "en"].map((locale) => ({
    url: `${siteUrl}/${locale}${path}`,
    alternates: { languages: { ru: `${siteUrl}/ru${path}`, en: `${siteUrl}/en${path}` } },
  })));
}
