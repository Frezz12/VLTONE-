import { expect, test } from "@playwright/test";

for (const locale of ["ru", "en"]) {
  test(`${locale} homepage exposes product metadata and the complete beta path`, async ({ page, request }) => {
    await page.goto(`/${locale}`);
    await expect(page).toHaveTitle(/VLTone.*(?:Открытая бета|Open beta)/);
    await expect(page.locator("h1")).toContainText("VLTone");
    await expect(page.locator('meta[name="description"]')).toHaveAttribute("content", /VLTone.*(?:DAW|digital audio workstation)/);
    await expect(page.locator('link[rel="canonical"]')).toHaveAttribute("href", `https://vltstudio.ru/${locale}`);
    await expect(page.locator('meta[property="og:site_name"]')).toHaveAttribute("content", "VLTone");
    await expect(page.locator('link[rel="alternate"][hreflang="ru"]')).toHaveAttribute("href", "https://vltstudio.ru/ru");
    const data = JSON.parse(await page.locator('script[type="application/ld+json"]').innerText());
    expect(data["@graph"].find((item: { "@type": string }) => item["@type"] === "SoftwareApplication")).toMatchObject({ name: "VLTone", applicationCategory: "MultimediaApplication", operatingSystem: "Windows, macOS" });

    const steps = page.locator(".beta-steps li");
    await expect(steps).toHaveCount(3);
    await expect(steps.nth(0).getByRole("link")).toHaveAttribute("href", `/${locale}/register`);
    await expect(steps.nth(1).getByRole("link")).toHaveAttribute("href", `/${locale}/releases`);
    await steps.nth(2).getByRole("link").click();
    await expect(page).toHaveURL(new RegExp(`/${locale}/manual#first-launch$`));
    await expect(page.locator("#first-launch")).toBeInViewport();
    await page.goto(`/${locale}`);
    await page.locator(".faq-list summary").first().focus();
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).toHaveAttribute("open", "");
    await page.keyboard.press("Enter");
    await expect(page.locator(".faq-list details").first()).not.toHaveAttribute("open");
    await page.locator(".hero-copy-panel").getByRole("link").first().click();
    await expect(page).toHaveURL(new RegExp(`/${locale}/register$`));
    await expect(page.locator('meta[name="robots"]')).toHaveAttribute("content", "noindex, follow");
    const ogImage = await request.get(`/${locale}/opengraph-image`);
    expect(ogImage.status()).toBe(200);
    expect(ogImage.headers()["content-type"]).toContain("image/png");
  });
}

for (const width of [320, 375, 768, 1440]) {
  test(`homepage fits ${width}px and preserves the reference tokens`, async ({ page }) => {
    await page.setViewportSize({ width, height: 900 });
    await page.goto("/ru");
    await expect(page.locator(".hero-product img")).toBeVisible();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    const style = await page.locator(".feature-card").first().evaluate((element) => {
      const computed = getComputedStyle(element);
      return { radius: computed.borderRadius, shadow: computed.boxShadow, weight: computed.fontWeight };
    });
    expect(style).toEqual({ radius: "54px", shadow: "none", weight: "350" });
    await expect(page.locator(".vlt-nav")).toBeVisible();
    await expect(page.locator(".locale-link")).toBeVisible();
  });
}

test("search discovery files expose both languages", async ({ request }) => {
  const robots = await request.get("/robots.txt");
  expect(await robots.text()).toContain("Sitemap: https://vltstudio.ru/sitemap.xml");
  const sitemap = await request.get("/sitemap.xml");
  const xml = await sitemap.text();
  for (const locale of ["ru", "en"]) {
    for (const path of ["", "/manual", "/releases"]) expect(xml).toContain(`<loc>https://vltstudio.ru/${locale}${path}</loc>`);
  }
  expect(xml).not.toContain("/account");
});
