import { ImageResponse } from "next/og";

export const alt = "VLTone — music creation software / Программа для создания музыки";
export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

export default async function OpenGraphImage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const ru = locale === "ru";
  return new ImageResponse(<div style={{ display: "flex", flexDirection: "column", justifyContent: "space-between", width: "100%", height: "100%", padding: "69px", background: "#101112", color: "#f2f4f6", fontWeight: 400 }}>
    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: 22 }}><span>vltstudio.ru</span><span style={{ border: "1px solid #bce0ff", color: "#bce0ff", borderRadius: 5, padding: "10px 24px" }}>{ru ? "Открытая бета" : "Open beta"}</span></div>
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}><span style={{ fontSize: 112, letterSpacing: -5, color: "#bce0ff" }}>VLTone</span><span style={{ fontSize: 36 }}>{ru ? "Твой звук." : "Your sound."}</span><span style={{ fontSize: 25, color: "#a4abb4" }}>{ru ? "Запись. Аранжировка. Сведение. AI-ассистент." : "Recording. Arranging. Mixing. An AI assistant."}</span></div>
    <span style={{ fontSize: 20, color: "#a4abb4" }}>Windows · macOS</span>
  </div>, size);
}
