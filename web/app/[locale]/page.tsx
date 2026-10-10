import type { Metadata } from "next";
import { HeroVideo } from "@/components/hero-video";
import Link from "next/link";
import { ArrowDown, ArrowUpRight, ArrowRight, Plus } from "lucide-react";
import { getTranslations, setRequestLocale } from "next-intl/server";
import { StudioGallery, ProductShot } from "@/components/product-gallery";
import { AmbientMotionControl } from "@/components/ambient-motion-control";
import { studioContent } from "@/lib/studio-content";
import { screenshotVersion } from "@/lib/screenshot-version";
import { siteMetadata, siteUrl } from "@/lib/seo";

function SoundGlyph({ kind = "wave" }: { kind?: "wave" | "notes" | "levels" }) {
  return <div className={"sound-glyph sound-glyph-" + kind} aria-hidden="true">
    {kind === "notes" ? <svg viewBox="0 0 280 72" fill="currentColor">
      {[0, 1, 2, 3, 4, 5, 6, 7].map(index => <rect key={index} className="note-pulse loop-motion" x={index * 35} y={42 - (index % 4) * 13} width={index % 3 === 0 ? 58 : 27} height="8" rx="2" style={{ animationDelay: (-index * .375) + "s" }} />)}
    </svg> : <div className="signal-bars">{Array.from({ length: kind === "levels" ? 18 : 42 }, (_, index) =>
      <i className="signal-bar loop-motion" key={index} style={{ height: (18 + Math.sin(index * .61) ** 2 * 82) + "%", animationDelay: (-index * .137) + "s", animationDuration: (1.6 + (index % 5) * .23) + "s" }} />
    )}</div>}
  </div>;
}

function MusicPhrase({ className }: { className: string }) {
  const melody = [[0, 4, 1.8], [2, 2, .8], [3, 3, .8], [4, 1, 2.8], [7, 2, .8], [8, 4, 1.8], [10, 3, .8], [11, 1, .8], [12, 0, 1.8], [14, 2, 1.8]];
  // One four-bar phrase at 120 BPM. Notes and transients share the cursor's clock.
  const pulseStyle = (beat: number) => ({ animationDelay: (beat * .5 - 8) + "s" });
  return <div className={"music-phrase " + className} aria-hidden="true">
    <svg viewBox="0 0 960 240" fill="none">
      <g className="phrase-grid">
        {Array.from({ length: 17 }, (_, beat) => <path key={beat} d={`M${24 + beat * 57} 12V228`} opacity={beat % 4 === 0 ? .55 : .22} />)}
        {[78, 96, 114, 132, 150, 168, 186].map(y => <path key={y} d={`M24 ${y}H936`} opacity=".2" />)}
      </g>
      <g className="phrase-audio">{Array.from({ length: 16 }, (_, beat) => {
        const envelope = Array.from({ length: 25 }, (_, sample) => {
          const height = 2 + Math.abs(Math.sin(sample * 2.4)) * Math.exp(-sample / 7) * (beat % 4 === 0 ? 32 : 21);
          return { x: 24 + beat * 57 + sample * 2.15, height };
        });
        const shape = envelope.map(({ x, height }) => `${x.toFixed(2)},${(44 - height).toFixed(2)}`).join(" ") + " " + envelope.toReversed().map(({ x, height }) => `${x.toFixed(2)},${(44 + height).toFixed(2)}`).join(" ");
        return <polygon key={beat} points={shape} className="phrase-hit loop-motion" style={pulseStyle(beat)} />;
      })}</g>
      <g className="phrase-melody">{melody.map(([beat, pitch, length]) =>
        <rect key={beat} x={24 + beat * 57} y={90 + pitch * 18} width={length * 57} height="8" rx="2" className="phrase-hit loop-motion" style={pulseStyle(beat)} />
      )}</g>
      <g className="phrase-bass">{[0, 4, 8, 12].map(beat =>
        <rect key={beat} x={24 + beat * 57} y={beat % 8 === 0 ? 212 : 202} width="207" height="5" rx="1" className="phrase-hit loop-motion" style={pulseStyle(beat)} />
      )}</g>
      <g className="phrase-cursor loop-motion">
        <path className="phrase-cursor-trail" d="M-20 12H0V228H-20Z" />
        <path d="M0 12V228" stroke="currentColor" strokeWidth="1.5" />
        <path d="M-5 9H5L0 16Z" fill="currentColor" />
      </g>
    </svg>
  </div>;
}

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
  const heroShot = "/images/studio/showcase-instrumental-" + (ru ? "ru" : "en") + ".webp?v=" + screenshotVersion;
  const structuredData = { "@context": "https://schema.org", "@graph": [
    { "@type": "WebSite", name: "VLTone", url: siteUrl, inLanguage: ["ru", "en"] },
    { "@type": "SoftwareApplication", name: "VLTone", url: siteUrl, description: t("metaDescription"), applicationCategory: "MultimediaApplication", operatingSystem: "Windows, macOS", featureList: c.workflow.map(item => item.title) },
  ] };

  return <main id="main-content" className="marketing-main studio-home" data-motion-paused="true" data-page-visible="false">
    <script type="application/ld+json" dangerouslySetInnerHTML={{ __html: JSON.stringify(structuredData).replace(/</g, "\\u003c") }} />

    <section className="studio-hero" aria-labelledby="hero-title" data-motion-scene>
      <div className="hero-stage">
        <div className="hero-art" aria-hidden="true">
          <div className="hero-screen">
            <HeroVideo locale={locale} poster={heroShot} />
          </div>
        </div>
        <div className="studio-container hero-content">
          <div className="hero-editorial">
            <h1 id="hero-title"><span>{ru ? "Дай форму" : "Give shape"}</span>{" "}<span>{ru ? "своему звуку." : "to your sound."}</span></h1>
            <div className="hero-copy-panel">
              <p>{c.intro}</p>
              <div className="hero-actions">
                <Link className="vlt-button" href="/releases">{c.start}<ArrowDown size={18} aria-hidden /></Link>
                <a className="text-link" href="#studio">{ru ? "Посмотреть программу" : "Explore the app"}<ArrowUpRight size={18} aria-hidden /></a>
              </div>
            </div>
          </div>
        </div>
      </div>
      <div className="studio-container hero-motion-footer"><SoundGlyph /><AmbientMotionControl locale={locale} /></div>
    </section>

    <section id="studio" className="studio-tour section-space" aria-labelledby="studio-title">
      <div className="studio-container">
      <header className="editorial-heading" data-reveal="0">
        <h2 id="studio-title">{ru ? "Весь проект" : "Your whole project"}<br /><span>{ru ? "перед глазами." : "in view."}</span></h2>
        <p>{ru ? "Аудиодорожки, MIDI-партии и автоматизация на одном таймлайне. Переключай экраны, чтобы рассмотреть аранжировку, редактор нот, микшер и эквалайзер." : "Audio tracks, MIDI parts and automation on one timeline. Switch views to explore the arrangement, note editor, mixer and equalizer."}</p>
      </header>
      <div data-reveal="60"><StudioGallery locale={locale} items={c.gallery} /></div>
      </div>
    </section>

    <section id="overview" className="studio-workflow" aria-labelledby="workflow-title">
      <div className="studio-container">
        <header className="workflow-heading" data-reveal="0">
          <h2 id="workflow-title">{ru ? "От записи до готового трека" : "From recording to the final mix"}</h2>
          <Link className="text-link" href="/capabilities">{c.allFeatures}<ArrowUpRight size={18} aria-hidden /></Link>
        </header>
      </div>
        <div className="workflow-grid">{c.workflow.map((item, index) =>
          <article className="workflow-card" data-track={index} key={item.id} data-motion-scene>
            <div className="studio-container workflow-inner" data-reveal="0">
            <div className="workflow-copy">
              <SoundGlyph kind={index === 0 ? "wave" : index === 1 ? "notes" : "levels"} />
              <h3>{item.title}</h3>
              <p>{item.copy}</p>
              <ul>{item.points.map(point => <li key={point}>{point}</li>)}</ul>
              <Link href={"/capabilities#" + item.id} className="text-link">{item.link}<ArrowRight size={17} aria-hidden /></Link>
            </div>
            <figure className="workflow-visual">
              <ProductShot locale={locale} name={item.shot} alt={item.alt} />
              <figcaption>{item.alt}</figcaption>
            </figure>
            </div>
          </article>
        )}</div>
    </section>

    <section className="studio-tools studio-container section-space" aria-labelledby="tools-title">
      <div data-reveal="0">
        <h2 id="tools-title">{ru ? "Дополняй студию" : "Make room"}<br /><span>{ru ? "своими инструментами." : "for your tools."}</span></h2>
        <p>{ru ? "Подключай привычные эффекты и синтезаторы. Помощник и восстановление проектов доступны прямо в программе." : "Connect the effects and synths you already use. An assistant and project recovery are built into the app."}</p>
      </div>
      <nav className="studio-extras" aria-label={c.allFeatures}>{c.extras.map((item, index) =>
        <Link href={item.href} key={item.title} data-reveal={index * 60}>
          <div><strong>{item.title}</strong><p>{item.copy}</p></div>
          <ArrowUpRight size={24} strokeWidth={1.4} aria-hidden />
        </Link>
      )}</nav>
    </section>

    <section id="beta" className="studio-onboarding" aria-labelledby="beta-title">
      <div className="studio-container section-space">
        <header className="editorial-heading" data-reveal="0">
          <h2 id="beta-title">{ru ? "Начни первый" : "Start your"}<br /><span>{ru ? "проект в VLTone." : "first VLTone project."}</span></h2>
          <p>{c.betaCopy}</p>
        </header>
        <ol className="beta-steps">{c.steps.map((step, index) =>
          <li key={step.href} data-reveal={index * 60}>
            <h3>{step.title}</h3><p>{step.copy}</p>
            <Link className="text-link" href={step.href}>{step.link}<ArrowUpRight size={16} aria-hidden /></Link>
          </li>
        )}</ol>
        <div className="beta-footnote"><p>{c.betaNote}</p><Link className="text-link" href="/bug-report">{t("reportBug")}<ArrowUpRight size={15} aria-hidden /></Link></div>
        <div className="studio-faq">
          <h2>{c.faqTitle}</h2>
          <div className="faq-list">{["platforms", "start", "beta"].map(key =>
            <details key={key}><summary>{t("faq." + key + ".question")}<Plus size={20} aria-hidden /></summary><p>{t("faq." + key + ".answer")}</p></details>
          )}</div>
        </div>
      </div>
    </section>

    <section className="studio-cta" aria-labelledby="cta-title" data-motion-scene>
      <MusicPhrase className="cta-phrase" />
      <div className="studio-container" data-reveal="0">
        <h2 id="cta-title">{ru ? "Открой VLTone." : "Open VLTone."}<br /><span>{ru ? "Запиши свою идею." : "Record your idea."}</span></h2>
        <div className="hero-actions">
          <Link className="vlt-button" href="/releases">{c.start}<ArrowDown size={18} aria-hidden /></Link>
          <Link className="text-link" href="/manual">{c.explore}<ArrowUpRight size={16} aria-hidden /></Link>
        </div>
      </div>
    </section>
  </main>;
}
