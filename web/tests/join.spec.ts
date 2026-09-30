import { expect, test } from "@playwright/test";

test("invitation stays in the fragment and offers app launch and code fallback", async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem("vlt-cookie-preference-v1", "necessary"));
  await page.context().grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.context().addCookies([{ name: "vlt-locale", value: "en", url: "http://127.0.0.1:3100" }]);
  const requests: string[] = [];
  page.on("request", (request) => requests.push(request.url()));
  await page.goto("/en/join#123456789012");
  await expect(page.getByRole("heading", { name: "Join a session" })).toBeVisible();
  await expect(page.getByRole("link", { name: "Open VLTONE" })).toHaveAttribute("href", "vlt://join/123456789012");
  await expect(page.getByLabel("Invitation code")).toHaveValue("123456789012");
  await page.getByRole("button", { name: "Copy code" }).click();
  await expect(page.getByRole("status")).toHaveText("Code copied.");
  await expect.poll(() => page.evaluate(() => navigator.clipboard.readText())).toBe("123456789012");
  await expect(page.getByRole("link", { name: "Download VLTONE" })).toHaveAttribute("href", "/releases?lang=en");
  expect(requests.some((url) => url.includes("123456789012"))).toBe(false);
  expect(requests.some((url) => /invites|redeem/.test(url))).toBe(false);

  await page.setViewportSize({ width: 375, height: 800 });
  await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.evaluate(() => { window.location.hash = "999999999999"; });
  await expect(page.getByRole("link", { name: "Open VLTONE" })).toHaveAttribute("href", "vlt://join/999999999999");
  await expect(page.getByRole("status")).toBeEmpty();
  await page.evaluate(() => { (document.activeElement as HTMLElement)?.blur(); window.scrollTo(0, 0); });
  await page.screenshot({ path: test.info().outputPath("join-mobile.png"), fullPage: true });
});

test("malformed and missing invitations cannot launch an app", async ({ page }) => {
  await page.context().addCookies([{ name: "vlt-locale", value: "ru", url: "http://127.0.0.1:3100" }]);
  for (const fragment of ["", "#12345", "#123456?command=other", "#１２３４５６", `#${"1".repeat(33)}`]) {
    await page.goto(`/join${fragment}`);
    await expect(page.getByRole("main").getByRole("alert")).toContainText("нет действительного кода");
    await expect(page.getByRole("link", { name: "Открыть VLTONE" })).toHaveCount(0);
    await expect(page.getByRole("button", { name: "Копировать код" })).toHaveCount(0);
  }
});
