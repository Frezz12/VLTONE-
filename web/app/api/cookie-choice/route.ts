import { NextRequest, NextResponse } from "next/server";

export function POST(request: NextRequest) {
  const origin = request.headers.get("origin");
  const host = request.headers.get("x-forwarded-host") ?? request.headers.get("host");
  if (origin && (!host || new URL(origin).host !== host)) return new Response(null, { status: 403 });

  const response = NextResponse.json({ cleared: true }, { headers: { "Cache-Control": "no-store" } });
  response.cookies.delete("vlt-locale");
  response.cookies.delete("vlt_web_session");
  response.cookies.delete("vlt_admin_session");
  return response;
}
