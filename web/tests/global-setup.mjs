import path from "node:path";
import { startManagedNodeServer } from "../../scripts/playwright-managed-server.mjs";

export default async function globalSetup() {
  const appRoot = path.resolve(import.meta.dirname, "..");
  const repositoryRoot = path.resolve(appRoot, "..");
  const stopApi = await startManagedNodeServer({
    name: "release test API",
    cwd: appRoot,
    args: ["tests/release-api.mjs"],
    url: "http://127.0.0.1:8099/health",
  });

  let stopWeb;
  try {
    stopWeb = await startManagedNodeServer({
      name: "web standalone server",
      cwd: appRoot,
      args: [path.join(repositoryRoot, "scripts", "run-next-standalone.mjs"), "web", "3100"],
      url: "http://127.0.0.1:3100/ru",
      env: { VLT_API_ORIGIN: "http://127.0.0.1:8099" },
    });
  } catch (error) {
    await stopApi();
    throw error;
  }

  return async () => {
    await stopWeb();
    await stopApi();
  };
}
