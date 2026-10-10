import type { Metadata } from "next";
import Link from "next/link";
import { notFound } from "next/navigation";
import { setRequestLocale } from "next-intl/server";
import { siteMetadata } from "@/lib/seo";
import { creatorNodes, findNode, categories, dynamicNodeIds } from "@/lib/creator/catalog";
import { CreatorDocsLayout } from "@/components/creator/docs-layout";
import { NodePorts } from "@/components/creator/node-ports";
import { PortSymbol } from "@/components/creator/port-symbol";
import { documentPort } from "@/lib/creator/port-copy";
import { nodeSettings } from "@/lib/creator/node-settings";
import type { Locale, DocumentedPort } from "@/lib/creator/types";

type Params = Promise<{ locale: Locale; id: string }>;
export function generateStaticParams() { return creatorNodes.map(node => ({ id: node.id })); }
export async function generateMetadata({ params }: { params: Params }): Promise<Metadata> {
  const { locale, id } = await params; const node = findNode(id); if (!node) notFound();
  return siteMetadata(locale, `/creator/docs/nodes/${id}`, `${node.name} — Creator / VLTone`, node.summary[locale]);
}
export default async function NodePage({ params }: { params: Params }) {
  const { locale, id } = await params; setRequestLocale(locale);
  const node = findNode(id); if (!node) notFound();
  const ru = locale === "ru", dynamic = dynamicNodeIds.has(id);
  const settings = nodeSettings(id);
  // These nodes have no fixed project-independent signature. Do not advertise the registry placeholder as a real port.
  const definitionOnly = ["cpp_function", "interface", "subgraph"].includes(id);
  const inputs = definitionOnly ? [] : node.inputs.map(port => documentPort(node, port, "input", locale));
  const outputs = definitionOnly ? [] : node.outputs.map(port => documentPort(node, port, "output", locale));
  const toc = [{ id: "ports", title: ru ? "Порты" : "Ports" }, { id: "parameters", title: ru ? "Параметры" : "Parameters" }, ...(settings.length ? [{ id: "settings", title: ru ? "Свойства" : "Properties" }] : []), { id: "example", title: ru ? "Пример" : "Example" }, { id: "notes", title: ru ? "Что проверить" : "Things to check" }];
  const renderPorts = (ports: DocumentedPort[], direction: "input" | "output") => ports.map(port => <tr key={`${direction}-${port.id}`} data-port-row={`${direction}-${port.id}`}><th scope="row"><span className="creator-port-cell"><PortSymbol type={port.type} />{port.name}</span><code>{port.id}</code></th><td>{port.type}</td><td><span>{port.description}</span><small>{direction === "output" ? (ru ? "Выход" : "Output") : port.required ? (ru ? "Обязательный" : "Required") : port.parameter >= 0 ? (ru ? "Ручное значение при отключении" : "Manual value when disconnected") : id === "history" && port.id === "write" ? (ru ? "По умолчанию включён" : "On by default") : (ru ? "Необязательный" : "Optional")}</small>{port.capacity > 0 && <small>{ru ? "Ёмкость" : "Capacity"}: {port.capacity}</small>}{port.signature && <code>{port.signature}</code>}</td></tr>);
  return <CreatorDocsLayout locale={locale} toc={toc}>
    <article className="creator-article creator-node-article"><header className="creator-doc-heading"><Link className="creator-eyebrow" href={`/creator/docs?category=${encodeURIComponent(node.category)}#catalog`}>CREATOR / {categories[node.category][locale]}</Link><h1>{node.name}</h1><code className="creator-node-id">{node.id} · v{node.version}</code><p>{node.summary[locale]}</p></header>
      <NodePorts name={node.name} inputs={inputs} outputs={outputs} dynamic={dynamic} locale={locale} />
      <section id="ports"><h2>{ru ? "Порты и подключения" : "Ports and connections"}</h2>{dynamic && <p className="creator-callout">{definitionOnly ? (ru ? "Порты этой ноды определяются проектом: ручками Interface, сигнатурой C++ или интерфейсом собственного подграфа." : "This node’s ports are defined by the project: Interface controls, the C++ signature or the custom subgraph interface.") : (ru ? "Ниже — исходная конфигурация. Типы и ёмкость могут изменяться в свойствах ноды; соединяй порты после выбора конфигурации." : "The default configuration is shown below. Types and capacity can change in node properties; configure the node before connecting ports.")}</p>}
      {!!(inputs.length + outputs.length) && <div className="creator-table-wrap" tabIndex={0} role="region" aria-label={ru ? "Таблица портов" : "Port table"}><table><thead><tr><th>{ru ? "Порт / ID" : "Port / ID"}</th><th>{ru ? "Тип" : "Type"}</th><th>{ru ? "Подключение" : "Connection"}</th></tr></thead><tbody>{renderPorts(inputs, "input")}{renderPorts(outputs, "output")}</tbody></table></div>}
      <p>{ru ? "Выход можно подключить к нескольким входам того же типа. У входа только один источник; Function требует совпадения сигнатуры." : "An output can feed multiple inputs of the same type. An input has one source; Function also requires matching signatures."} <Link href="/creator/docs/guides/ports">{ru ? "Все правила соединений →" : "All connection rules →"}</Link></p></section>
      <section id="parameters"><h2>{ru ? "Параметры" : "Parameters"}</h2>{node.parameters.length ? <><p>{ru ? "Значения относятся к ручным настройкам. Подключённый поток заменяет ручное значение параметра; после отключения оно восстанавливается." : "Values describe manual settings. A connected stream replaces a parameter’s manual value; disconnecting restores it."}</p><div className="creator-table-wrap" tabIndex={0} role="region" aria-label={ru ? "Таблица параметров" : "Parameter table"}><table className="creator-parameter-table"><thead><tr><th>{ru ? "Параметр" : "Parameter"}</th><th>{ru ? "Диапазон" : "Range"}</th><th>Default</th><th>{ru ? "Управление" : "Control"}</th></tr></thead><tbody>{node.parameters.map(p => <tr key={p.id}><th scope="row">{p.name}<code>{p.id}</code></th><td>{p.minimum}…{p.maximum}{p.unit && ` ${p.unit}`}{p.choices.length > 0 && <small>{p.choices.map((choice, i) => `${p.minimum + i}: ${choice}`).join(" · ")}</small>}</td><td>{p.initial}{p.unit && ` ${p.unit}`}</td><td>{inputs.some(port => port.parameter === node.parameters.indexOf(p)) ? (ru ? "Вход или вручную" : "Input or manual") : (ru ? "Статический" : "Static")}<small>{p.logarithmic ? (ru ? "Логарифмическая шкала" : "Logarithmic scale") : (ru ? "Линейная шкала" : "Linear scale")}</small></td></tr>)}</tbody></table></div></> : <p>{ru ? "Фиксированных числовых параметров нет. Для собственных нод, коллекций и C++ дополнительные настройки задаются определением ноды в инспекторе." : "There are no fixed numeric parameters. Custom nodes, collections and C++ expose additional settings through their definition in the inspector."}</p>}</section>
      {!!settings.length && <section id="settings"><h2>{ru ? "Свойства ноды" : "Node properties"}</h2><p>{ru ? "Эти настройки задаются до компиляции и не модулируются проводами." : "These settings are configured before compilation and cannot be modulated by wires."}</p><dl className="creator-node-settings">{settings.map(setting => <div key={setting.name}><dt>{setting.name}</dt><dd><strong>{setting.value[locale]}</strong><p>{setting.detail[locale]}</p></dd></div>)}</dl></section>}
      <section id="example"><h2>{ru ? "Как использовать" : "How to use it"}</h2><p>{node.example[locale]}</p></section>
      <section id="notes"><h2>{ru ? "Что проверить" : "Things to check"}</h2>{node.note && <p className="creator-callout">{node.note[locale]}</p>}<p>{ru ? "Проверь обязательные входы и соответствие типов. Перед прослушиванием выполни Compile; ошибка проверки сохраняет прежний работающий модуль." : "Check required inputs and matching types. Run Compile before auditioning; a validation failure preserves the previous working module."} <Link href="/creator/docs/guides/troubleshooting">{ru ? "Диагностика →" : "Troubleshooting →"}</Link></p></section>
      <nav className="creator-related" aria-label={ru ? "Связанные ноды" : "Related nodes"}>{Array.from(new Set(node.related)).map(related => <Link href={`/creator/docs/nodes/${related}`} key={related}>{findNode(related)!.name}<span aria-hidden>↗</span></Link>)}</nav>
    </article>
  </CreatorDocsLayout>;
}
