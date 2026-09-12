import { ImageResponse } from "next/og";

export const alt = "VLTone — music creation software / Программа для создания музыки";
export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

export default async function OpenGraphImage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const ru = locale === "ru";
  return new ImageResponse(<div style={{ display: "flex", flexDirection: "column", justifyContent: "space-between", width: "100%", height: "100%", padding: "69px", background: "#f2f2f4", color: "#0f1012", fontWeight: 400 }}>
    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: 22 }}><span>vltstudio.ru</span><span style={{ border: "1px solid #0071e3", color: "#0071e3", borderRadius: 40, padding: "10px 24px" }}>{ru ? "Открытая бета" : "Open beta"}</span></div>
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}><span style={{ fontSize: 112, letterSpacing: -5 }}>VLTone</span><span style={{ fontSize: 36 }}>{ru ? "Программа для создания музыки." : "A space for your music."}</span><span style={{ fontSize: 25, color: "#5e5e5e" }}>{ru ? "Запись. Аранжировка. Сведение. AI-ассистент." : "Recording. Arranging. Mixing. An AI assistant."}</span></div>
    <span style={{ fontSize: 20, color: "#5e5e5e" }}>Windows · macOS</span>
  </div>, size);
}
