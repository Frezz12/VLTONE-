import { expect, test } from "@playwright/test";

const releaseID = "10000000-0000-4000-8000-000000000001";

test("release upload proxy streams artifacts and screenshots to a versioned API origin", async ({ request }) => {
  const artifact = await request.put(`/release-upload/v1/admin/releases/${releaseID}/artifacts/windows-exe`, {
    headers: { "X-CSRF-Token": "csrf", Origin: "http://127.0.0.1:3101", Cookie: "vlt_admin_session=test" },
    multipart: { file: { name: "VLT-Setup.exe", mimeType: "application/octet-stream", buffer: Buffer.from("installer") } },
  });
  expect(artifact.ok()).toBe(true);
  await expect(artifact.json()).resolves.toMatchObject({
    method: "PUT",
    path: `/v1/admin/releases/${releaseID}/artifacts/windows-exe`,
  });

  const screenshot = await request.post(`/release-upload/v1/admin/releases/${releaseID}/screenshots`, {
    headers: { "X-CSRF-Token": "csrf", Origin: "http://127.0.0.1:3101", Cookie: "vlt_admin_session=test" },
    multipart: {
      file: { name: "mixer.png", mimeType: "image/png", buffer: Buffer.from("png") },
      caption_ru: "Микшер",
      caption_en: "Mixer",
    },
  });
  expect(screenshot.ok()).toBe(true);
  await expect(screenshot.json()).resolves.toMatchObject({
    method: "POST",
    path: `/v1/admin/releases/${releaseID}/screenshots`,
  });
});
