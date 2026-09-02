import path from "node:path";
import { startManagedNodeServer } from "../../scripts/playwright-managed-server.mjs";

export default async function globalSetup() {
  const appRoot = path.resolve(import.meta.dirname, "..");
  const repositoryRoot = path.resolve(appRoot, "..");
  return startManagedNodeServer({
    name: "admin standalone server",
    cwd: appRoot,
    args: [path.join(repositoryRoot, "scripts", "run-next-standalone.mjs"), "admin", "3101"],
    url: "http://127.0.0.1:3101/login",
  });
}
