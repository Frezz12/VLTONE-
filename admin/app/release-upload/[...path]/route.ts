import { NextRequest } from "next/server";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const allowed = /^v1\/admin\/releases\/[0-9a-f-]+\/(artifacts\/[a-z-]+|screenshots)$/;

async function forward(request: NextRequest, context: { params: Promise<{ path: string[] }> }) {
  const path = (await context.params).path.join("/");
  if (!allowed.test(path)) return Response.json({ code: "upload_route_invalid", message: "Upload route is not allowed." }, { status: 404 });
  const origin = (process.env.VLT_API_ORIGIN ?? "http://localhost:8080")
    .replace(/\/+$/, "")
    .replace(/\/v1$/, "");
  const headers = new Headers();
  for (const name of ["content-type", "content-length", "cookie", "origin", "x-csrf-token"]) {
    const value = request.headers.get(name);
    if (value) headers.set(name, value);
  }
  const init: RequestInit & { duplex: "half" } = {
    method: request.method,
    headers,
    body: request.body,
    redirect: "manual",
    duplex: "half",
  };
  try {
    const response = await fetch(`${origin}/${path}`, init);
    const responseHeaders = new Headers();
    for (const name of ["content-type", "content-length", "x-request-id"]) {
      const value = response.headers.get(name);
      if (value) responseHeaders.set(name, value);
    }
    return new Response(response.body, { status: response.status, headers: responseHeaders });
  } catch {
    return Response.json({ code: "upload_unavailable", message: "Сервис загрузки недоступен." }, { status: 502 });
  }
}

export const PUT = forward;
export const POST = forward;
