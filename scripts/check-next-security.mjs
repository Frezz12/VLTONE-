import { readFile } from "node:fs/promises";

const release = process.env.VLT_PUBLIC_RELEASE === "1" || process.env.GITHUB_REF_TYPE === "tag";
if (!release) {
  process.stdout.write("Next.js public-release security gate: development build allowed.\n");
  process.exit(0);
}

const blocked = new Set(["16.2.11"]);
for (const project of ["web", "admin"]) {
  const manifest = JSON.parse(await readFile(new URL(`../${project}/package.json`, import.meta.url), "utf8"));
  const version = manifest.dependencies?.next;
  if (!version || blocked.has(version)) {
    throw new Error(`${project} uses Next.js ${version ?? "unknown"}. Public releases are blocked until the security patch announced for 2026-08-26 is pinned and reviewed.`);
  }
}
process.stdout.write("Next.js public-release security gate passed.\n");
