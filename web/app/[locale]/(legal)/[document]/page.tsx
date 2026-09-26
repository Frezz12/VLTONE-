import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { setRequestLocale } from "next-intl/server";
import { ArrowLeft, ArrowUpRight } from "lucide-react";
import { legalDocuments, legalProfile, legalReady, legalTitles, legalVersion, type LegalDocument } from "@/lib/legal";
import { legalContent } from "@/lib/legal-content";
import { siteMetadata } from "@/lib/seo";

function documentKey(value: string): LegalDocument {
  if (!legalDocuments.includes(value as LegalDocument)) notFound();
  return value as LegalDocument;
}
export function generateStaticParams() { return legalDocuments.map(document => ({ document })); }
export async function generateMetadata({ params }: { params: Promise<{ locale: string; document: string }> }): Promise<Metadata> {
  const { locale, document } = await params;
  const title = legalTitles[locale === "ru" ? "ru" : "en"][documentKey(document)];
  return { ...siteMetadata(locale, `/${document}`, `${title} — VLTone`, title), robots: { index: legalReady, follow: true } };
}
export default async function LegalPage({ params, searchParams }: { params: Promise<{ locale: string; document: string }>; searchParams: Promise<{ version?: string }> }) {
  const { locale, document } = await params;
  const { version } = await searchParams;
  if (version && version !== legalVersion) notFound();
  setRequestLocale(locale);
  const key = documentKey(document);
  const ru = locale === "ru";
  const titles = legalTitles[ru ? "ru" : "en"];
  const content = legalContent(key, locale);
  return <main id="main-content" className="legal-main">
    <Link className="text-link" href="/register"><ArrowLeft size={16} aria-hidden />{ru ? "К регистрации" : "Back to registration"}</Link>
    <header className="legal-heading"><span className="section-label">VLTone / {ru ? "Документы" : "Legal"} / {legalVersion}</span><h1>{titles[key]}</h1><p>{content.intro}</p></header>
    {!legalReady && <aside className="legal-draft" role="note">{ru ? "Почтовый адрес оператора, сведения об уведомлении Роскомнадзора и используемых сервисах ожидают подтверждения." : "The operator's postal address, Roskomnadzor notification and service-provider details are awaiting confirmation."}</aside>}
    <div className="legal-layout"><aside className="legal-toc"><span className="section-label">{ru ? "В этом документе" : "On this page"}</span><nav aria-label={ru ? "Разделы документа" : "Document sections"}>{content.sections.map((section, index) => <a key={section.title} href={`#section-${index + 1}`}>{String(index + 1).padStart(2, "0")} {section.title}</a>)}</nav><div className="legal-other">{legalDocuments.filter(value => value !== key).map(value => <Link key={value} href={`/${value}`}>{titles[value]}<ArrowUpRight size={13} aria-hidden /></Link>)}</div></aside>
    <article className="legal-body">
      <section className="legal-operator"><span className="section-label">{ru ? "Оператор и владелец сайта" : "Data operator and website owner"}</span><strong>{legalProfile.operator_name}</strong><a href={`mailto:${legalProfile.contact_email}`}>{legalProfile.contact_email}</a><p>{ru ? "Адрес для корреспонденции: " : "Postal address: "}{legalProfile.operator_address || (ru ? "ожидает заполнения оператором" : "pending completion by the operator")}</p></section>
      {content.sections.map((section, index) => <section key={section.title} id={`section-${index + 1}`}><h2><span>{String(index + 1).padStart(2, "0")}</span>{section.title}</h2>{section.paragraphs.map(paragraph => <p key={paragraph}>{paragraph}</p>)}</section>)}
      <div className="legal-contact"><h2>{ru ? "Вопрос о своих данных?" : "A question about your data?"}</h2><p>{ru ? "Напишите оператору с почты, указанной в аккаунте. Для первого обращения не нужно отправлять копию паспорта." : "Contact the operator from your account email. You do not need to attach a passport copy to your first enquiry."}</p><a className="text-link" href={`mailto:${legalProfile.contact_email}`}>{legalProfile.contact_email}<ArrowUpRight size={16} aria-hidden /></a></div>
    </article></div>
  </main>;
}
