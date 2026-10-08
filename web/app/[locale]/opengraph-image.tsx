import { ImageResponse } from "next/og";

export const alt = "VLTone — music creation software / Программа для создания музыки";
export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

export default async function OpenGraphImage({ params }: { params: Promise<{ locale: string }> }) {
  const { locale } = await params;
  const ru = locale === "ru";
  return new ImageResponse(<div style={{ display: "flex", flexDirection: "column", justifyContent: "space-between", width: "100%", height: "100%", padding: "69px", background: "linear-gradient(120deg, #171a32 25%, #35305d)", color: "#f4f1e9", fontWeight: 400 }}>
    <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: 22 }}><span>vltstudio.ru</span><span style={{ border: "1px solid #c4b5ee", color: "#c4b5ee", borderRadius: 5, padding: "10px 24px" }}>{ru ? "Открытая бета" : "Open beta"}</span></div>
    <div style={{ display: "flex", flexDirection: "column", gap: 18 }}><span style={{ fontSize: 112, letterSpacing: -5, color: "#d5edaa" }}>VLTone</span><span style={{ fontSize: 42 }}>{ru ? "Дай форму своему звуку." : "Give shape to your sound."}</span><span style={{ fontSize: 25, color: "#a8b1bd" }}>{ru ? "Запись. Аранжировка. Сведение." : "Recording. Arranging. Mixing."}</span></div>
    <span style={{ fontSize: 20, color: "#a4abb4" }}>Windows · macOS</span>
  </div>, size);
}
