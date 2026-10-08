import type { Metadata } from "next";
import Link from "next/link";
import { ArrowDown, ArrowUpRight, Plus } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { ProductShot } from "@/components/product-gallery";
import { siteMetadata, siteUrl } from "@/lib/seo";

const capabilityKeys = ["recording", "midi", "mixing", "plugins", "ai", "recovery"] as const;
const capabilityShots = { recording: "showcase-instrumental", midi: "showcase-midi", mixing: "showcase-mix", plugins: "plugins", ai: "ai", recovery: "recovery" };

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
    <div className="studio-container">
      <section className="capabilities-hero" aria-labelledby="capabilities-title">
        <div className="capabilities-topline">
          <span className="section-label">{t("eyebrow")}</span>
          <span className="capabilities-beta status-dot">{t("statusLabel")}</span>
        </div>
        <div className="capabilities-intro" data-reveal="0">
          <h1 id="capabilities-title">{t("title")}<span>{t("titleAccent")}</span></h1>
          <div className="capabilities-intro-copy">
            <p>{t("intro")}</p>
            <Link className="vlt-button" href="/releases">{t("download")}<ArrowUpRight size={17} aria-hidden /></Link>
            <span className="platform-caption">Windows & macOS</span>
          </div>
        </div>
      </section>

      <nav className="capabilities-toc" aria-label={t("contentsLabel")}>
        {capabilityKeys.map((key) => <a href={`#${key}`} key={key}>
          {t(`items.${key}.nav`)}<ArrowDown size={13} aria-hidden />
        </a>)}
      </nav>

      <div id="capability-list" className="capability-list">
        {capabilityKeys.map((key, index) => {
          const points = t.raw(`items.${key}.points`) as string[];
          const highlights = t.raw(`items.${key}.highlights`) as string[];
          const primary = index < 3;
          const visual = <figure className="capability-visual">
            <ProductShot locale={locale} name={capabilityShots[key]} alt={t(`items.${key}.caption`)} eager={index === 0} />
            <figcaption>{t(`items.${key}.caption`)}</figcaption>
          </figure>;

          return <article id={key} className={`capability-detail ${primary ? "capability-primary" : "capability-extra"}`} aria-labelledby={`${key}-title`} key={key} data-reveal="0">
            {!primary && visual}
            <div className="capability-copy">
              <h2 id={`${key}-title`}>{t(`items.${key}.title`)}</h2>
              <p className="capability-lead">{t(`items.${key}.lead`)}</p>
              <p className="capability-highlights">{highlights.map((highlight) => <span key={highlight}>{highlight}</span>)}</p>
              <details className="capability-specs">
                <summary>{t("details")}<Plus size={16} aria-hidden /></summary>
                <ul>{points.map((point) => <li key={point}>{point}</li>)}</ul>
              </details>
              <Link className="text-link" href={`/manual#${t(`items.${key}.manualAnchor`)}`}>{t("manualLink")}<ArrowUpRight size={14} aria-hidden /></Link>
            </div>
            {primary && visual}
          </article>;
        })}
      </div>

      <section className="capabilities-cta" aria-labelledby="capabilities-cta-title">
        <div><span className="section-label">{t("ctaLabel")}</span><h2 id="capabilities-cta-title">{t("ctaTitle")}</h2><p>{t("ctaCopy")}</p></div>
        <div className="hero-actions"><Link className="vlt-button" href="/releases">{t("download")}<ArrowUpRight size={17} aria-hidden /></Link><Link className="text-link" href="/manual">{t("guide")}<ArrowUpRight size={14} aria-hidden /></Link></div>
      </section>
    </div>
  </main>;
}
