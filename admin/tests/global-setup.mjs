import path from "node:path";
import { startManagedNodeServer } from "../../scripts/playwright-managed-server.mjs";

export default async function globalSetup() {
  const appRoot = path.resolve(import.meta.dirname, "..");
  const repositoryRoot = path.resolve(appRoot, "..");
  const stopApi = await startManagedNodeServer({
    name: "release upload test API",
    cwd: appRoot,
    args: ["tests/release-upload-api.mjs"],
    url: "http://127.0.0.1:8098/health",
  });

  let stopAdmin;
  try {
    stopAdmin = await startManagedNodeServer({
      name: "admin standalone server",
      cwd: appRoot,
      args: [path.join(repositoryRoot, "scripts", "run-next-standalone.mjs"), "admin", "3101"],
      url: "http://127.0.0.1:3101/login",
      env: { VLT_API_ORIGIN: "http://127.0.0.1:8098/v1" },
    });
  } catch (error) {
    await stopApi();
    throw error;
  }

  return async () => {
    await stopAdmin();
    await stopApi();
  };
}
