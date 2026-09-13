import { expect, test } from "@playwright/test";

for (const locale of ["ru", "en"]) {
  test(`${locale} homepage exposes product metadata and the complete beta path`, async ({ page, request }) => {
    await page.context().addCookies([{ name: "vlt-locale", value: locale, url: "http://127.0.0.1:3100" }]);
    await page.goto("/");
    await expect(page).toHaveTitle(/VLTone.*(?:Открытая бета|Open beta)/);
    await expect(page.locator("h1")).toContainText("VLTone");
    await expect(page.locator(".hero-announcement")).toHaveCount(0);
    await expect(page.locator(".vlt-brand .brand-mark img")).toBeVisible();
    await expect(page.locator('.cta-sound span')).toHaveCount(4);
    await expect(page.locator('link[rel="icon"]')).toHaveAttribute("href", "/icon.png");
    await expect(page.locator('meta[name="description"]')).toHaveAttribute("content", /VLTone.*(?:DAW|digital audio workstation)/);
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute("href", "https://vltstudio.ru");
    await expect(page.locator('meta[property="og:site_name"]')).toHaveAttribute("content", "VLTone");
    await expect(page.locator('link[rel="alternate"][hreflang]')).toHaveCount(0);
    const data = JSON.parse(await page.locator('script[type="application/ld+json"]').innerText());
    expect(data["@graph"].find((item: { "@type": string }) => item["@type"] === "SoftwareApplication")).toMatchObject({ name: "VLTone", applicationCategory: "MultimediaApplication", operatingSystem: "Windows, macOS" });

    const steps = page.locator(".beta-steps li");
    await expect(steps).toHaveCount(3);
    await expect(steps.nth(0).getByRole("link")).toHaveAttribute("href", "/register");
    await expect(steps.nth(1).getByRole("link")).toHaveAttribute("href", "/releases");
    await steps.nth(2).getByRole("link").click();
    await expect(page).toHaveURL(/\/manual#first-launch$/);
    await expect(page.locator("#first-launch")).toBeInViewport();
    await page.goto("/");
    await page.locator(".faq-list summary").first().focus();
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).toHaveAttribute("open", "");
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).not.toHaveAttribute("open");
    await page.locator(".hero-copy-panel").getByRole("link").first().click();
    await expect(page).toHaveURL(/\/register$/);
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute("content", "noindex, follow");
    const ogImage = await request.get("/opengraph-image");
    expect(ogImage.status()).toBe(200);
    expect(ogImage.headers()["content-type"]).toContain("image/png");
  });
}

for (const width of [320, 375, 768, 1440]) {
  test(`homepage fits ${width}px and preserves the refreshed visual system`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/");
    await expect(page.locator(".hero-product img")).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const style = await page.locator(".feature-card").first().evaluate((element) => {
      const computed = getComputedStyle(element);
      return { radius: computed.borderRadius, shadow: computed.boxShadow, weight: computed.fontWeight };
    });
    expect(parseFloat(style.radius)).toBeGreaterThanOrEqual(26);
    expect(style.shadow).not.toBe("none");
    expect(style.weight).toBe("350");
    await expect(page.locator(".vlt-nav")).toBeVisible();
    await expect(page.locator(".locale-link")).toBeVisible();
  });
}

test("homepage respects reduced motion", async ({ page }) => {
  await page.emulateMedia({ reducedMotion: "reduce" });
  await page.goto("/");
  const motion = await page.locator(".cta-sound span").first().evaluate((element) => {
    const style = getComputedStyle(element);
    return { duration: style.animationDuration, iterations: style.animationIterationCount };
  });
  expect(motion.iterations).toBe("1");
  expect(parseFloat(motion.duration)).toBeLessThan(.001);
});

test("search discovery files expose clean canonical routes", async ({ request }) => {
  const robots = await request.get("/robots.txt");
  expect(await robots.text()).toContain("Sitemap: https://vltstudio.ru/sitemap.xml");
  const sitemap = await request.get("/sitemap.xml");
  const xml = await sitemap.text();
  for (const path of ["", "/manual", "/releases"]) expect(xml).toContain(`<loc>https://vltstudio.ru${path}</loc>`);
  expect(xml).not.toMatch(/vltstudio\.ru\/(ru|en)(\/|<)/);
  expect(xml).not.toContain("/account");
});

test("language preference persists without changing the URL", async ({ page }) => {
  await page.goto("/en/manual");
  await expect(page).toHaveURL(/\/manual$/);
  await expect(page.getByRole("heading", { name: "VLTone Manual" })).toBeVisible();
  await page.getByRole("button", { name: "Открыть на русском" }).click();
  await expect(page).toHaveURL(/\/manual$/);
  await expect(page.getByRole("heading", { name: "Инструкция VLTone" })).toBeVisible();
  await page.reload();
  await expect(page.getByRole("heading", { name: "Инструкция VLTone" })).toBeVisible();
});
