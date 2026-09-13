import { NextRequest, NextResponse } from "next/server";
import { locales, type Locale } from "./i18n/request";

const localeCookie = "vlt-locale";
const internalRewriteHeader = "x-vlt-locale-rewrite";

export default function proxy(request: NextRequest) {
  const { pathname } = request.nextUrl;
  const segment = pathname.split("/")[1];

  if (request.headers.get(internalRewriteHeader) === "1") return NextResponse.next();

  // Keep old localized links working, but never expose the locale in the
  // canonical URL. Visiting one also updates the saved language preference.
  if (locales.includes(segment as (typeof locales)[number])) {
    const target = request.nextUrl.clone();
    target.pathname = pathname.slice(segment.length + 1) || "/";
    const response = NextResponse.redirect(target);
    response.cookies.set(localeCookie, segment, { maxAge: 60 * 60 * 24 * 365, path: "/", sameSite: "lax" });
    return response;
  }

  const savedLocale = request.cookies.get(localeCookie)?.value;
  const locale: Locale = locales.includes(savedLocale as Locale) ? savedLocale as Locale : "ru";
  const target = request.nextUrl.clone();
  target.protocol = "http:";
  target.pathname = `/${locale}${pathname === "/" ? "" : pathname}`;
  const headers = new Headers(request.headers);
  headers.set(internalRewriteHeader, "1");
  const response = NextResponse.rewrite(target, { request: { headers } });
  if (!savedLocale) response.cookies.set(localeCookie, locale, { maxAge: 60 * 60 * 24 * 365, path: "/", sameSite: "lax" });
  return response;
}

export const config = {
  matcher: ["/releases/:path*", "/ru/releases/:path*", "/en/releases/:path*", "/((?!api|_next|.*\\..*).*)"],
};
