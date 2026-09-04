import { cp, mkdir, readFile, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";
import process from "node:process";

const [appName, portText] = process.argv.slice(2);
if (!new Set(["web", "admin"]).has(appName)) {
  throw new Error("usage: run-next-standalone.mjs <web|admin> <port>");
}
const port = Number(portText);
if (!Number.isInteger(port) || port < 1 || port > 65535) {
  throw new Error("standalone port must be an integer between 1 and 65535");
}

const repositoryRoot = path.resolve(import.meta.dirname, "..");
const appRoot = path.join(repositoryRoot, appName);
const standaloneRoot = path.join(appRoot, ".next", "standalone");
const candidates = [
  path.join(standaloneRoot, appName, "server.js"),
  path.join(standaloneRoot, "server.js"),
];

let serverEntry;
for (const candidate of candidates) {
  try {
    if ((await stat(candidate)).isFile()) {
      serverEntry = candidate;
      break;
    }
  } catch {
    // Try the next canonical standalone layout.
  }
}
if (!serverEntry) {
  throw new Error(`${appName} standalone server.js is missing; run its build first`);
}

const runtimeRoot = path.dirname(serverEntry);
await mkdir(path.join(runtimeRoot, ".next", "static"), { recursive: true });
await cp(path.join(appRoot, ".next", "static"),
         path.join(runtimeRoot, ".next", "static"),
         { recursive: true, force: true });
try {
  await cp(path.join(appRoot, "public"), path.join(runtimeRoot, "public"),
           { recursive: true, force: true });
} catch (error) {
  if (error?.code !== "ENOENT") throw error;
}

if (process.env.VLT_API_ORIGIN) {
  const apiOrigin = new URL(process.env.VLT_API_ORIGIN);
  if (!new Set(["http:", "https:"]).has(apiOrigin.protocol)) {
    throw new Error("VLT_API_ORIGIN must use http or https");
  }
  const manifestPath = path.join(runtimeRoot, ".next", "routes-manifest.json");
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  const rewriteGroups = Object.values(manifest.rewrites ?? {});
  const apiRewrite = rewriteGroups.flat().find((rewrite) => rewrite.source === "/api/:path*");
  if (!apiRewrite) throw new Error("standalone API rewrite is missing");
  const apiRoot = apiOrigin.toString().replace(/\/+$/, "").replace(/\/v1$/, "");
  apiRewrite.destination = `${apiRoot}/:path*`;
  await writeFile(manifestPath, JSON.stringify(manifest), "utf8");
}

process.chdir(runtimeRoot);
process.env.HOSTNAME = "127.0.0.1";
process.env.PORT = String(port);
await import(pathToFileURL(serverEntry).href);
for (const signal of ["SIGINT", "SIGTERM"]) {
  process.once(signal, () => process.exit(0));
}
