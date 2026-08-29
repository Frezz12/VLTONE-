"use client";

import type { ReleaseArtifact } from "@/lib/releases";
import { Apple, Check, Clipboard, Download, Laptop, Monitor, X } from "lucide-react";
import { useRef, useState } from "react";

const quarantineCommand = 'sudo xattr -rd com.apple.quarantine "/Applications/VLT Studio Pro.app"';

function readableBytes(value: number) {
  if (value >= 1024 ** 3) return `${(value / 1024 ** 3).toFixed(2)} GiB`;
  return `${(value / 1024 ** 2).toFixed(1)} MiB`;
}

export function ReleaseDownloads({ artifacts, locale }: { artifacts: ReleaseArtifact[]; locale: string }) {
  const dialog = useRef<HTMLDialogElement>(null);
  const [macArtifact, setMacArtifact] = useState<ReleaseArtifact>();
  const [copied, setCopied] = useState(false);
  const ru = locale === "ru";

  function warnMac(item: ReleaseArtifact) {
    setMacArtifact(item); setCopied(false); dialog.current?.showModal();
  }
  async function copyCommand() {
    await navigator.clipboard.writeText(quarantineCommand);
    setCopied(true);
  }

  return <>
    <div className="release-download-grid">
      {artifacts.map((item) => {
        const Icon = item.platform === "windows" ? Monitor : item.platform === "macos" ? Apple : Laptop;
        const content = <><Icon size={20} aria-hidden /><span><strong>{item.label}</strong><small>{item.file_name} · {readableBytes(item.bytes)}</small></span><Download size={18} aria-hidden /></>;
        return item.platform === "macos"
          ? <button className="release-download" type="button" onClick={() => warnMac(item)} key={item.kind}>{content}</button>
          : <a className="release-download" href={`/api${item.download_url}`} key={item.kind}>{content}</a>;
      })}
    </div>
    <dialog className="mac-download-dialog" ref={dialog} aria-labelledby="mac-warning-title" aria-describedby="mac-warning-copy" onClose={() => setMacArtifact(undefined)}>
      <div className="mac-dialog-head"><Apple size={22} aria-hidden /><h2 id="mac-warning-title">{ru ? "Перед запуском на macOS" : "Before launching on macOS"}</h2><button type="button" onClick={() => dialog.current?.close()} aria-label={ru ? "Закрыть" : "Close"}><X size={19} aria-hidden /></button></div>
      <p id="mac-warning-copy">{ru ? "Сборка пока не нотарифицирована Apple. Загружайте её только с официального сайта, перенесите приложение в папку Applications, затем выполните в Terminal:" : "This build is not yet notarized by Apple. Download it only from the official site, move the app to Applications, then run this command in Terminal:"}</p>
      <div className="mac-command"><code>{quarantineCommand}</code><button className="vlt-button vlt-button-secondary" type="button" onClick={() => void copyCommand()}>{copied ? <Check size={16} aria-hidden /> : <Clipboard size={16} aria-hidden />}{copied ? (ru ? "Скопировано" : "Copied") : (ru ? "Копировать" : "Copy")}</button></div>
      <div className="mac-dialog-actions"><button className="vlt-button vlt-button-secondary" type="button" onClick={() => dialog.current?.close()}>{ru ? "Отмена" : "Cancel"}</button>{macArtifact && <a className="vlt-button" href={`/api${macArtifact.download_url}`} onClick={() => dialog.current?.close()}><Download size={16} aria-hidden />{ru ? "Скачать DMG" : "Download DMG"}</a>}</div>
    </dialog>
  </>;
}
