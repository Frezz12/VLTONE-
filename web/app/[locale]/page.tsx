import Image from "next/image";
import Link from "next/link";
import { ArrowRight, AudioWaveform, Bot, ShieldCheck, Waves } from "lucide-react";
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
      <div className="hero-copy-panel">
        <span className="hero-kicker">{t("kicker")}</span>
        <div className="hero-heading">
          <h1 className="hero-title">{t("title")}</h1>
          <p className="hero-copy">{t("copy")}</p>
        </div>
        <div className="hero-actions">
          <Link className="vlt-button" href={`/${locale}/register`}>{t("create")}<ArrowRight size={16} aria-hidden /></Link>
          <Link className="vlt-button vlt-button-secondary" href={`/${locale}/login`}>{t("login")}</Link>
        </div>
      </div>
      <figure className="hero-product">
        <Image src={`/manual/${locale}/arrangement.png`} width={1440} height={1200} priority alt={t("featureDawTitle")} />
        <figcaption>VLT Studio Pro · {locale === "ru" ? "Рабочая область" : "Desktop workspace"}</figcaption>
      </figure>
    </section>

    <section className="home-features">
      <header className="home-section-head"><span>01</span><div><h2>{t("featuresTitle")}</h2><p>{t("featuresCopy")}</p></div></header>
      <div className="feature-grid">{features.map(([Icon, title, copy], index) => <article className="feature-card" key={title}>
        <span className="feature-index">0{index + 1}</span><Icon size={20} aria-hidden /><div><h3>{title}</h3><p>{copy}</p></div>
      </article>)}</div>
    </section>

    <section className="home-cta"><span>VLT Studio Pro</span><h2>{t("ctaTitle")}</h2><Link className="vlt-button" href={`/${locale}/register`}>{t("create")}<ArrowRight size={16} aria-hidden /></Link></section>
    <footer className="home-footer"><span>VLT Studio Pro © 2026</span><span>{t("footer")}</span></footer>
  </main>;
}
