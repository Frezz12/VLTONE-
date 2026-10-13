import { defineConfig, devices } from "@playwright/test";
const apiOrigin = process.env.VLT_API_ORIGIN || "http://127.0.0.1:8080";
export default defineConfig({
  testDir: "./tests",
  workers: 1,
  timeout: 45_000,
  expect: { timeout: 10_000 },
  reporter: "list",
  use: {
    baseURL: "http://localhost:3022",
    trace: "retain-on-failure",
    contextOptions: { reducedMotion: "reduce" },
    launchOptions: process.env.PLAYWRIGHT_EXECUTABLE_PATH
      ? { executablePath: process.env.PLAYWRIGHT_EXECUTABLE_PATH }
      : undefined,
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    {
      name: "mobile",
      use: { ...devices["iPhone 13"], defaultBrowserType: "chromium" },
    },
  ],
  webServer: [
    {
      command: "pnpm exec next dev -p 3022",
      url: "http://localhost:3022",
      reuseExistingServer: false,
      timeout: 120_000,
      env: { VLT_API_ORIGIN: apiOrigin },
    },
    ...(process.env.VLT_FORUM_E2E
      ? [
          {
            command: "pnpm --filter @vlt/web exec next dev -p 3020",
            url: "http://127.0.0.1:3020",
            reuseExistingServer: false,
            timeout: 120_000,
            env: { VLT_API_ORIGIN: apiOrigin },
          },
        ]
      : []),
  ],
});
