import type { Metadata } from "next";
import Image from "next/image";
import Link from "next/link";
import { ArrowDown, ArrowUpRight, Plus } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { siteMetadata, siteUrl } from "@/lib/seo";

export async function generateMetadata({ params }: { params: Promise<{ locale: string }> }): Promise<Metadata> {
  const { locale } = await params;
  const t = await getTranslations({ locale, namespace: "Home" });
  return siteMetadata(locale, "", t("metaTitle"), t("metaDescription"));
}

export default async function Home({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  setRequestLocale(locale);
  const t = await getTranslations("Home");
  const features = ["recording", "midi", "mixing", "plugins", "ai", "recovery"] as const;
  const steps = ["register", "download", "signin"] as const;
  const stepLinks = [`/${locale}/register`, `/${locale}/releases`, `/${locale}/manual#first-launch`] as const;
  const questions = ["what", "who", "beta", "start", "platforms", "feedback"] as const;
  const structuredData = {
    "@context": "https://schema.org",
    "@graph": [
      { "@type": "WebSite", "@id": `${siteUrl}/#website`, name: "VLTone", url: siteUrl, inLanguage: ["ru", "en"] },
      {
        "@type": "SoftwareApplication", "@id": `${siteUrl}/#software`, name: "VLTone",
        url: `${siteUrl}/${locale}`, description: t("metaDescription"),
        applicationCategory: "MultimediaApplication", applicationSubCategory: "Digital Audio Workstation",
        operatingSystem: "Windows, macOS", inLanguage: ["ru", "en"],
        releaseNotes: `${siteUrl}/${locale}/releases`,
        featureList: features.map((key) => t(`features.${key}.title`)),
      },
    ],
  };

  return <main id="main-content" className="marketing-main">
    <script type="application/ld+json" dangerouslySetInnerHTML={{ __html: JSON.stringify(structuredData).replace(/</g, "\\u003c") }} />
    <section className="hero-frame" aria-labelledby="hero-title">
      <figure className="hero-product"><Image src={`/images/workspace-dark-${locale}.png`} width={1600} height={1000} sizes="(max-width: 700px) 100vw, (max-width: 1200px) 58vw, 730px" priority alt={t("productAlt")} /><figcaption>{t("productCaption")}</figcaption></figure>
      <Link className="hero-announcement" href="#beta"><span className="pill-tag">{t("betaTag")}</span><span>{t("announcement")}</span><ArrowUpRight size={14} aria-hidden /></Link>
      <div className="hero-copy-panel">
        <span className="section-label">{t("kicker")}</span>
        <h1 id="hero-title">VLTone<span>{t("title")}</span></h1>
        <p>{t("copy")}</p>
        <div className="hero-actions">
          <Link className="vlt-button" href={`/${locale}/register`}><ArrowUpRight size={15} aria-hidden />{t("create")}</Link>
          <Link className="text-link" href="#overview">{t("discover")}<ArrowDown size={14} aria-hidden /></Link>
        </div>
      </div>
      <div className="hero-caption"><span>{t("heroCaption")}</span><span>Windows · macOS</span></div>
    </section>

    <section id="overview" className="home-section overview-section" aria-labelledby="overview-title">
      <span className="section-label">01 / {t("overviewLabel")}</span>
      <div><h2 id="overview-title">{t("overviewTitle")}</h2><Link className="text-link" href={`/${locale}/manual`}>{t("manual")}<ArrowUpRight size={14} aria-hidden /></Link></div>
      <div className="overview-copy"><p>{t("overviewCopy")}</p><p>{t("audienceCopy")}</p></div>
    </section>

    <section id="features" className="home-section" aria-labelledby="features-title">
      <header className="section-heading"><div><span className="section-label">02 / {t("featuresLabel")}</span><h2 id="features-title">{t("featuresTitle")}</h2></div><p>{t("featuresCopy")}</p></header>
      <div className="feature-grid">{features.map((key, index) => <article className="feature-card" key={key}>
        <span className="feature-index">0{index + 1}</span>
        <h3>{t(`features.${key}.title`)}</h3><p>{t(`features.${key}.copy`)}</p>
        <Link className="feature-link" href={`/${locale}/manual#${t(`features.${key}.anchor`)}`}><span className="pill-tag">{t(`features.${key}.tag`)}</span><ArrowUpRight size={14} aria-hidden /><span className="sr-only"> — {t(`features.${key}.title`)}</span></Link>
      </article>)}</div>
    </section>

    <section id="beta" className="home-section beta-section" aria-labelledby="beta-title">
      <div className="beta-intro"><span className="section-label">03 / {t("betaLabel")}</span><span className="pill-tag">{t("betaTag")}</span><h2 id="beta-title">{t("betaTitle")}</h2><p>{t("betaCopy")}</p></div>
      <ol className="beta-steps">{steps.map((key, index) => <li key={key}>
        <span className="step-number">0{index + 1}</span><h3>{t(`steps.${key}.title`)}</h3><p>{t(`steps.${key}.copy`)}</p>
        <Link className="text-link" href={stepLinks[index]}>{t(`steps.${key}.link`)}<ArrowUpRight size={14} aria-hidden /></Link>
      </li>)}</ol>
      <div className="beta-note"><p>{t("betaNote")}</p><Link className="text-link" href={`/${locale}/bug-report`}>{t("reportBug")}<ArrowUpRight size={14} aria-hidden /></Link></div>
    </section>

    <section id="faq" className="home-section faq-section" aria-labelledby="faq-title">
      <div><span className="section-label">04 / {t("faqLabel")}</span><h2 id="faq-title">{t("faqTitle")}</h2></div>
      <div className="faq-list">{questions.map((key) => <details key={key}><summary>{t(`faq.${key}.question`)}<Plus size={17} aria-hidden /></summary><p>{t(`faq.${key}.answer`)}</p></details>)}</div>
    </section>

    <section className="home-section home-cta" aria-labelledby="cta-title"><span className="section-label">{t("ctaLabel")}</span><h2 id="cta-title">{t("ctaTitle")}</h2><div className="hero-actions"><Link className="vlt-button" href={`/${locale}/register`}><ArrowUpRight size={15} aria-hidden />{t("create")}</Link><Link className="text-link" href={`/${locale}/releases`}>{t("download")}<ArrowUpRight size={14} aria-hidden /></Link></div></section>
  </main>;
}
