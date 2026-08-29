export type ReleaseArtifact = {
  id: string;
  kind: "windows-exe" | "macos-dmg" | "linux-appimage" | "linux-deb" | "linux-rpm" | "linux-tar-gz" | "linux-tar-xz";
  platform: "windows" | "macos" | "linux";
  label: string;
  file_name: string;
  bytes: number;
  sha256: string;
  download_url: string;
  updated_at: string;
};

export type ReleaseScreenshot = { id: string; caption: string; sort_order: number; width: number; height: number; sha256: string; url: string };
export type PublicRelease = {
  id: string; version: string; summary: string; features: string[]; changes: string[]; fixes: string[];
  artifacts: ReleaseArtifact[]; screenshots: ReleaseScreenshot[]; page_url: string; published_at: string;
};

const origin = () => (process.env.VLT_API_ORIGIN ?? "http://localhost:8080").replace(/\/$/, "");

export async function getReleases(locale: string) {
  const response = await fetch(`${origin()}/v1/releases?locale=${encodeURIComponent(locale)}`, { cache: "no-store" });
  if (!response.ok) throw new Error(`Release API failed (${response.status})`);
  return (await response.json() as { releases: PublicRelease[] }).releases;
}

export async function getRelease(locale: string, version: string) {
  const response = await fetch(`${origin()}/v1/releases/${encodeURIComponent(version)}?locale=${encodeURIComponent(locale)}`, { cache: "no-store" });
  if (response.status === 404) return undefined;
  if (!response.ok) throw new Error(`Release API failed (${response.status})`);
  return response.json() as Promise<PublicRelease>;
}
