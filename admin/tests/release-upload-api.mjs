import { createServer } from "node:http";

const artifactPath = /^\/v1\/admin\/releases\/[0-9a-f-]+\/artifacts\/[a-z-]+$/;
const screenshotPath = /^\/v1\/admin\/releases\/[0-9a-f-]+\/screenshots$/;

createServer((request, response) => {
  const url = new URL(request.url, "http://127.0.0.1:8098");
  if (url.pathname === "/health") return response.end("ok");

  const allowed = (request.method === "PUT" && artifactPath.test(url.pathname))
    || (request.method === "POST" && screenshotPath.test(url.pathname));
  if (!allowed) {
    response.writeHead(404, { "Content-Type": "application/json" });
    return response.end(JSON.stringify({ code: "not_found", path: url.pathname }));
  }

  let bytes = 0;
  request.on("data", (chunk) => { bytes += chunk.length; });
  request.on("end", () => {
    const contentType = request.headers["content-type"] ?? "";
    const valid = contentType.startsWith("multipart/form-data; boundary=")
      && request.headers["x-csrf-token"] === "csrf"
      && request.headers.origin === "http://127.0.0.1:3101"
      && request.headers.cookie === "vlt_admin_session=test"
      && bytes > 0;
    response.writeHead(valid ? 200 : 400, { "Content-Type": "application/json" });
    response.end(JSON.stringify({ method: request.method, path: url.pathname, bytes, content_type: contentType }));
  });
}).listen(8098, "127.0.0.1");
