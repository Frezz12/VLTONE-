import { NextRequest, NextResponse } from "next/server";
import { locales } from "./i18n/request";

// Keep locale selection at the edge deliberately small. The previous
// next-intl middleware created an internal rewrite loop in a production
// Next.js 16 reverse-proxy deployment. Pages validate their locale too;
// this layer only supplies the deterministic / -> /ru redirect.
export default function proxy(request: NextRequest) {
  const { pathname } = request.nextUrl;
  const segment = pathname.split("/")[1];
  if (locales.includes(segment as (typeof locales)[number])) {
    return NextResponse.next();
  }

  const target = request.nextUrl.clone();
  target.pathname = pathname === "/" ? "/ru" : `/ru${pathname}`;
  return NextResponse.redirect(target);
}

export const config = { matcher: ["/((?!api|_next|.*\\..*).*)"] };
