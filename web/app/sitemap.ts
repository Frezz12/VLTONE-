import type { MetadataRoute } from "next";
import { legalReady, legalDocuments } from "@/lib/legal";
import { siteUrl } from "@/lib/seo";

export default function sitemap(): MetadataRoute.Sitemap {
  return ["", "/capabilities", "/manual", "/releases", ...(legalReady ? legalDocuments.map(path => `/${path}`) : [])].map((path) => ({ url: `${siteUrl}${path}` }));
}
