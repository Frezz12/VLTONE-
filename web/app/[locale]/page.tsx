import Link from "next/link";
import { ArrowUpRight, AudioWaveform, Bot, ShieldCheck, Waves } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";

export default async function Home({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  setRequestLocale(locale);
  const t = await getTranslations("Home");
  const features = [
    [AudioWaveform, t("featureDawTitle"), t("featureDawCopy")],
    [Bot, t("featureAiTitle"), t("featureAiCopy")],
    [Waves, t("featureRecoveryTitle"), t("featureRecoveryCopy")],
    [ShieldCheck, t("featureControlTitle"), t("featureControlCopy")],
  ] as const;
  return <main className="marketing-main">
    <section className="hero-frame">
      <div className="hero-grid">
        <div className="hero-copy-panel">
          <div className="hero-topline"><span className="hero-kicker">{t("kicker")}</span><span className="hero-index">[01—04]</span></div>
          <div className="hero-heading">
            <h1 className="hero-title">{t("title")}</h1>
            <p className="hero-copy">{t("copy")}</p>
            <div className="hero-actions"><Link className="vlt-button" href={`/${locale}/register`}>{t("create")}<ArrowUpRight size={16} aria-hidden /></Link><Link className="vlt-button vlt-button-secondary" href={`/${locale}/login`}>{t("login")}</Link></div>
          </div>
          <div className="hero-facts"><div className="hero-fact"><strong>20M</strong><span>{t("factTokens")}</span></div><div className="hero-fact"><strong>∞</strong><span>{t("factAccess")}</span></div><div className="hero-fact"><strong>24/7</strong><span>{t("factRecovery")}</span></div></div>
        </div>
        <div className="hero-visual" aria-hidden>
          <div className="studio-window">
            <div className="studio-bar"><span>VLT / Session 001</span><span className="studio-lights"><i /><i /><i /></span><span>128 BPM</span></div>
            <div className="studio-timeline"><div className="studio-tracks"><span>DRM</span><span>BAS</span><span>SYN</span><span>VOX</span><span>FX</span></div><div className="studio-clips"><i /><i /><i /><i /><i /><span className="studio-playhead" /></div></div>
            <div className="studio-mixer">{[62, 78, 46, 86, 54, 71].map((level) => <span className="studio-channel" style={{ "--level": `${level}%` } as React.CSSProperties} key={level} />)}</div>
          </div>
          <span className="hero-visual-label">DIGITAL AUDIO WORKSTATION / 2026</span>
        </div>
      </div>
    </section>

    <section className="home-features">
      <header className="home-section-head"><h2>{t("featuresTitle")}</h2><p>{t("featuresCopy")}</p></header>
      <div className="feature-grid">{features.map(([Icon, title, copy], index) => <article className="feature-card" key={title}><span className="feature-icon"><Icon size={22} aria-hidden /></span><div><span className="hero-index">0{index + 1}</span><h3>{title}</h3><p>{copy}</p></div></article>)}</div>
    </section>

    <section className="home-cta"><h2>{t("ctaTitle")}</h2><Link className="vlt-button" href={`/${locale}/register`}>{t("create")}<ArrowUpRight size={16} aria-hidden /></Link></section>
    <footer className="home-footer"><span>VLT Studio Pro © 2026</span><span>{t("footer")}</span></footer>
  </main>;
}
