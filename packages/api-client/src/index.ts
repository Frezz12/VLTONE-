export type APIError = {
  code: string;
  message: string;
  field_errors?: Record<string, string>;
  request_id?: string;
};

export type { components, operations, paths } from "./schema";

export type User = {
  id: string;
  email: string;
  nickname: string;
  locale: "ru" | "en";
  status: "active" | "suspended";
  collaboration_enabled: boolean;
  consent_version: string;
  consent_accepted_at: string;
  created_at: string;
};

export type Quota = {
  base_limit: number;
  adjustment: number;
  used_tokens: number;
  reserved_tokens: number;
  remaining_tokens: number;
  starts_at: string;
  ends_at: string;
};

export type Device = {
  id: string;
  install_id: string;
  display_name: string;
  platform: string;
  os_version: string;
  app_version: string;
  hardware: Record<string, unknown>;
  first_seen_at: string;
  last_seen_at: string;
  revoked_at?: string | null;
};

export type AccountSession = {
  user: User;
  csrf_token: string;
  subscription: { plan: "demo"; display_name: "Demo"; all_features: true };
  quota: Quota;
};

export class VltApiClient {
  constructor(private readonly prefix = "/api") {}

  async request<T>(path: string, init: RequestInit = {}): Promise<T> {
    const response = await fetch(`${this.prefix}${path}`, {
      ...init,
      credentials: "include",
      headers: { Accept: "application/json", ...init.headers },
    });
    if (!response.ok) {
      const fallback: APIError = { code: "request_failed", message: `Request failed (${response.status})` };
      throw (await response.json().catch(() => fallback)) as APIError;
    }
    if (response.status === 204) return undefined as T;
    return response.json() as Promise<T>;
  }

  json<T>(path: string, method: string, body: unknown, csrf?: string) {
    return this.request<T>(path, {
      method,
      headers: { "Content-Type": "application/json", ...(csrf ? { "X-CSRF-Token": csrf } : {}) },
      body: JSON.stringify(body),
    });
  }
}

export const api = new VltApiClient();
