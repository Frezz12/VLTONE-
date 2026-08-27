const lastActivityKey = "vlt-admin-last-activity";
const pollingActivityWindowMs = 25 * 60 * 1000;

export function markAdminActivity() {
  window.localStorage.setItem(lastActivityKey, String(Date.now()));
}

export function adminPollingAllowed() {
  if (document.visibilityState !== "visible") return false;
  const last = Number(window.localStorage.getItem(lastActivityKey));
  return Number.isFinite(last) && Date.now() - last < pollingActivityWindowMs;
}
