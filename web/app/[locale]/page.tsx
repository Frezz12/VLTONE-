import type { Metadata } from "next";
import Link from "next/link";
import { ArrowDown, ArrowUpRight, ArrowRight, AudioLines, Piano, SlidersHorizontal, Plus, Headphones, Presentation, UsersRound, Link2, MousePointer2 } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { StudioGallery, ProductShot } from "@/components/product-gallery";
import { studioContent } from "@/lib/studio-content";
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
  const c = studioContent(locale);
  const ru = locale === "ru";
  const icons = [AudioLines, Piano, SlidersHorizontal];
  const modeIcons = [Headphones, Presentation, UsersRound];
  const structuredData = { "@context": "https://schema.org", "@graph": [
    { "@type": "WebSite", name: "VLTone", url: siteUrl, inLanguage: ["ru", "en"] },
    { "@type": "SoftwareApplication", name: "VLTone", url: siteUrl, description: t("metaDescription"), applicationCategory: "MultimediaApplication", operatingSystem: "Windows, macOS", featureList: c.workflow.map(item => item.title) },
  ] };

  return <main id="main-content" className="marketing-main studio-home">
    <script type="application/ld+json" dangerouslySetInnerHTML={{ __html: JSON.stringify(structuredData).replace(/</g, "\\u003c") }} />
    <section className="studio-hero" aria-labelledby="hero-title">
      <div className="studio-container">
        <div className="hero-topline"><span className="section-label">{c.eyebrow}</span><span className="status-dot">{t("betaTag")}</span></div>
        <div className="hero-editorial">
          <h1 id="hero-title">{c.title}</h1>
          <div className="hero-copy-panel"><p>{c.intro}</p><div className="hero-actions"><Link className="vlt-button" href="/releases">{c.start}<ArrowDown size={18} aria-hidden /></Link><Link className="text-link" href="/manual">{c.explore}<ArrowUpRight size={16} aria-hidden /></Link></div><span className="platform-caption">Windows & macOS</span></div>
        </div>
        <StudioGallery locale={locale} items={c.gallery} />
        <div className="studio-proof"><span>{ru ? "Скриншоты сборки 0.3.1 · готовится к выпуску" : "Screenshots from build 0.3.1 · in development"}</span><Link href="/releases">{ru ? "Доступные версии и изменения" : "Available versions & changelog"}<ArrowUpRight size={15} aria-hidden /></Link></div>
      </div>
      <div className="format-strip" aria-label={c.compatibility}><span>AUDIO + MIDI</span><span>VST / VST3</span><span>CLAP</span><span>AU <small>macOS</small></span><span>{c.export}</span></div>
    </section>
    <section id="overview" className="studio-light" aria-labelledby="workflow-title">
      <div className="studio-container section-space">
        <header className="editorial-heading"><div><span className="section-label">01 / {c.workflowLabel}</span><h2 id="workflow-title">{c.workflowTitle}</h2></div><Link className="text-link" href="/capabilities">{c.allFeatures}<ArrowUpRight size={18} aria-hidden /></Link></header>
        <div className="workflow-grid">{c.workflow.map((item, index) => {
          const Icon = icons[index];
          return <article className="workflow-card" key={item.id}>
            <ProductShot locale={locale} name={item.shot} alt={item.alt} />
            <div className="workflow-copy"><span className="workflow-number">0{index + 1}<Icon size={20} strokeWidth={1.5} aria-hidden /></span><h3>{item.title}</h3><p>{item.copy}</p><Link href={`/capabilities#${item.id}`} className="text-link">{item.link}<ArrowRight size={16} aria-hidden /></Link></div>
          </article>;
        })}</div>
      </div>
    </section>
    <section id="collaboration" className="studio-collaboration studio-container section-space" aria-labelledby="collaboration-title">
      <header className="editorial-heading"><div><span className="section-label">02 / {c.collaboration.label}</span><h2 id="collaboration-title">{c.collaboration.title}</h2></div><p>{c.collaboration.copy}</p></header>
      <div className="collaboration-stage">
        <figure className="session-preview">
          <div className="session-board" role="img" aria-label={c.collaboration.previewAlt}>
            <div aria-hidden="true">
              <div className="session-board-header"><span>{c.collaboration.previewLabel}</span><div className="session-avatars"><span>{c.collaboration.you}</span><span>A</span><span>M</span></div></div>
              <div className="session-ruler"><span>01</span><span>05</span><span>09</span><span>13</span><span>17</span></div>
              <div className="session-tracks">{c.collaboration.tracks.map((track, index) => <div className="session-track" key={track}>
                <span className="session-track-name"><span>0{index + 1}</span>{track}</span>
                <div className="session-lane"><span className="session-clip"><i /><i /><i /><i /><i /><i /><i /><i /></span><span className="session-clip"><i /><i /><i /><i /><i /><i /><i /><i /></span></div>
              </div>)}<div className="session-cursor session-cursor-you"><MousePointer2 size={21} fill="currentColor" /><span>{c.collaboration.you}</span></div><div className="session-cursor session-cursor-other"><MousePointer2 size={21} fill="currentColor" /><span>{c.collaboration.collaborator}</span></div></div>
              <div className="session-board-footer"><AudioLines size={17} /><span>AUDIO + MIDI</span><span>120 BPM / 4:4</span></div>
            </div>
          </div>
          <figcaption>{c.collaboration.previewCaption}</figcaption>
        </figure>
        <div className="collaboration-invite"><Link2 size={28} strokeWidth={1.5} aria-hidden /><span className="section-label">{c.collaboration.inviteLabel}</span><h3>{c.collaboration.inviteTitle}</h3><p>{c.collaboration.inviteCopy}</p><Link className="text-link" href="#beta">{c.collaboration.start}<ArrowRight size={17} aria-hidden /></Link></div>
      </div>
      <div className="collaboration-modes">{c.collaboration.modes.map((mode, index) => {
        const Icon = modeIcons[index];
        return <article key={mode.title}><span className="collaboration-mode-label"><span>0{index + 1}</span><Icon size={21} strokeWidth={1.5} aria-hidden /></span><h3>{mode.title}</h3><p>{mode.copy}</p></article>;
      })}</div>
      <p className="collaboration-version">{c.collaboration.versionNote}</p>
      <div className="studio-extras">{c.extras.map(item => <Link href={item.href} key={item.title}><span>{item.tag}</span><strong>{item.title}</strong><ArrowUpRight size={20} aria-hidden /></Link>)}</div>
    </section>
    <section id="beta" className="studio-light" aria-labelledby="beta-title">
      <div className="studio-container section-space">
        <header className="editorial-heading"><div><span className="section-label">03 / {c.betaLabel}</span><h2 id="beta-title">{c.betaTitle}</h2></div><p>{c.betaCopy}</p></header>
        <ol className="beta-steps">{c.steps.map((step, index) => <li key={step.href}><span className="step-index">0{index + 1}</span><h3>{step.title}</h3><p>{step.copy}</p><Link className="text-link" href={step.href}>{step.link}<ArrowUpRight size={16} aria-hidden /></Link></li>)}</ol>
        <div className="beta-footnote"><span className="status-dot">{t("betaTag")}</span><p>{c.betaNote}</p><Link className="text-link" href="/bug-report">{t("reportBug")}<ArrowUpRight size={15} aria-hidden /></Link></div>
        <div className="studio-faq"><h2>{c.faqTitle}</h2><div className="faq-list">{["platforms", "start", "beta"].map(key => <details key={key}><summary>{t(`faq.${key}.question`)}<Plus size={18} aria-hidden /></summary><p>{t(`faq.${key}.answer`)}</p></details>)}</div></div>
      </div>
    </section>
    <section className="studio-cta studio-container" aria-labelledby="cta-title"><span className="section-label">Windows / macOS</span><h2 id="cta-title">{c.cta}</h2><div className="hero-actions"><Link className="vlt-button" href="/releases">{c.start}<ArrowDown size={18} aria-hidden /></Link><Link className="text-link" href="/manual">{c.explore}<ArrowUpRight size={16} aria-hidden /></Link></div></section>
  </main>;
}
