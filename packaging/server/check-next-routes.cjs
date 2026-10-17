const { readFileSync } = require("node:fs");
const { createRequire } = require("node:module");
const path = require("node:path");

const appRoot = path.resolve(process.argv[2]);
const appRequire = createRequire(path.join(appRoot, "server.js"));
process.env.NODE_ENV = "production";
appRequire("next/dist/server/next");

// Starting Next or rendering a page does not load the upload route's runtime.
const serverRoot = path.join(appRoot, ".next", "server");
const routes = JSON.parse(readFileSync(path.join(serverRoot, "app-paths-manifest.json"), "utf8"));
for (const [name, file] of Object.entries(routes)) {
  if (!name.endsWith("/route")) continue;
  appRequire(path.join(serverRoot, file));
  console.log(`Loaded Next route ${name}`);
}
