import type { MetadataRoute } from "next";
import { legalReady, legalDocuments } from "@/lib/legal";
import { siteUrl } from "@/lib/seo";
import { creatorNodes } from "@/lib/creator/catalog";
import { guides } from "@/lib/creator/guides";

export default function sitemap(): MetadataRoute.Sitemap {
  return ["", "/capabilities", "/manual", "/releases", "/creator", "/creator/docs", ...guides.map(guide => `/creator/docs/guides/${guide.slug}`), ...creatorNodes.map(node => `/creator/docs/nodes/${node.id}`), ...(legalReady ? legalDocuments.map(path => `/${path}`) : [])].map((path) => ({ url: `${siteUrl}${path}` }));
}
