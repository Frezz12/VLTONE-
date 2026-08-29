import { defineConfig, devices } from "@playwright/test";

export default defineConfig({
  testDir: "./tests",
  fullyParallel: true,
  retries: process.env.CI ? 2 : 0,
  reporter: process.env.CI ? "github" : "list",
  use: { baseURL: "http://127.0.0.1:3100", trace: "retain-on-failure", launchOptions: process.env.PLAYWRIGHT_EXECUTABLE_PATH ? { executablePath: process.env.PLAYWRIGHT_EXECUTABLE_PATH } : undefined },
  projects: [{ name: "chromium", use: { ...devices["Desktop Chrome"] } }],
  webServer: [
    { command: "node tests/release-api.mjs", url: "http://127.0.0.1:8099/health", reuseExistingServer: true },
    { command: "corepack pnpm start -p 3100", url: "http://127.0.0.1:3100/ru", reuseExistingServer: !process.env.CI, timeout: 120_000, env: { ...process.env, VLT_API_ORIGIN: "http://127.0.0.1:8099" } },
  ],
});
