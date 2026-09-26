export type CookiePreference = "necessary" | "none";
export type CookiePromptReason = "account" | null;

const storageKey = "vlt-cookie-preference-v1";

export function readCookiePreference(): CookiePreference | null {
  try {
    const value = window.localStorage.getItem(storageKey);
    return value === "necessary" || value === "none" ? value : null;
  } catch {
    return null;
  }
}

export function saveCookiePreference(value: CookiePreference) {
  try { window.localStorage.setItem(storageKey, value); } catch { /* The choice still applies until reload. */ }
  window.dispatchEvent(new Event("vlt-cookie-preference-change"));
}

export function requestCookieChoice(reason: CookiePromptReason = null) {
  window.dispatchEvent(new CustomEvent("vlt-cookie-choice-request", { detail: reason }));
}
