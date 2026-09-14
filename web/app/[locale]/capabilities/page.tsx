import type { Metadata } from "next";
import Link from "next/link";
import { ArrowDown, ArrowUpRight, History, Mic2, Piano, PlugZap, SlidersHorizontal, Sparkles } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { siteMetadata, siteUrl } from "@/lib/seo";

const capabilityKeys = ["recording", "midi", "mixing", "plugins", "ai", "recovery"] as const;
const capabilityIcons = { recording: Mic2, midi: Piano, mixing: SlidersHorizontal, plugins: PlugZap, ai: Sparkles, recovery: History };

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  const t = await getTranslations({ locale, namespace: "Capabilities" });
  return siteMetadata(locale, "/capabilities", t("metaTitle"), t("metaDescription"));
}

export default async function CapabilitiesPage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  setRequestLocale(locale);
  const t = await getTranslations("Capabilities");
  const structuredData = {
    "@context": "https://schema.org",
    "@type": "SoftwareApplication",
    name: "VLTone",
    url: `${siteUrl}/capabilities`,
    applicationCategory: "MultimediaApplication",
    operatingSystem: "Windows, macOS",
    description: t("metaDescription"),
    featureList: capabilityKeys.map((key) => t(`items.${key}.title`)),
  };

  return <main id="main-content" className="capabilities-main">
    <script type="application/ld+json" dangerouslySetInnerHTML={{ __html: JSON.stringify(structuredData).replace(/</g, "\\u003c") }} />
    <section className="capabilities-hero" aria-labelledby="capabilities-title">
      <div data-reveal>
        <span className="section-label">{t("eyebrow")}</span>
        <h1 id="capabilities-title">{t("title")}</h1>
        <p>{t("intro")}</p>
        <div className="hero-actions">
          <Link className="vlt-button" href="/register"><ArrowUpRight size={15} aria-hidden />{t("join")}</Link>
          <Link className="text-link" href="#capability-list">{t("explore")}<ArrowDown size={14} aria-hidden /></Link>
        </div>
      </div>
      <aside className="capabilities-beta" data-reveal="80">
        <span className="pill-tag">{t("statusLabel")}</span>
        <h2>{t("statusTitle")}</h2>
        <p>{t("statusCopy")}</p>
        <Link className="text-link" href="/releases">{t("releases")}<ArrowUpRight size={14} aria-hidden /></Link>
      </aside>
    </section>

    <section id="capability-list" className="capabilities-directory" aria-labelledby="directory-title">
      <header className="capabilities-directory-heading" data-reveal>
        <span className="section-label">{t("directoryLabel")}</span>
        <h2 id="directory-title">{t("directoryTitle")}</h2>
        <p>{t("directoryCopy")}</p>
      </header>
      <nav className="capabilities-toc" aria-label={t("contentsLabel")} data-reveal>
        {capabilityKeys.map((key) => <a href={`#${key}`} key={key}>{t(`items.${key}.title`)}</a>)}
      </nav>
      <div className="capability-list">
        {capabilityKeys.map((key, index) => {
          const Icon = capabilityIcons[key];
          const points = t.raw(`items.${key}.points`) as string[];
          return <article id={key} className="capability-detail" key={key} data-reveal={String(index * 45)}>
            <span className="capability-detail-icon"><Icon size={29} strokeWidth={1.4} aria-hidden /></span>
            <div>
              <span className="pill-tag">{t(`items.${key}.tag`)}</span>
              <h2>{t(`items.${key}.title`)}</h2>
              <p className="capability-lead">{t(`items.${key}.lead`)}</p>
              <ul>{points.map((point) => <li key={point}>{point}</li>)}</ul>
              <Link className="text-link" href={`/manual#${t(`items.${key}.manualAnchor`)}`}>{t("manualLink")}<ArrowUpRight size={14} aria-hidden /></Link>
            </div>
          </article>;
        })}
      </div>
    </section>

    <section className="capabilities-cta" aria-labelledby="capabilities-cta-title" data-reveal>
      <div><span className="section-label">{t("ctaLabel")}</span><h2 id="capabilities-cta-title">{t("ctaTitle")}</h2><p>{t("ctaCopy")}</p></div>
      <div className="hero-actions"><Link className="vlt-button" href="/releases"><ArrowUpRight size={15} aria-hidden />{t("download")}</Link><Link className="text-link" href="/manual">{t("guide")}<ArrowUpRight size={14} aria-hidden /></Link></div>
    </section>
  </main>;
}
