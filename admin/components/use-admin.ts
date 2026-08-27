"use client";
import type { APIError } from "@vlt/api-client";
import { api } from "@vlt/api-client";
import { useRouter } from "next/navigation";
import { useEffect, useState } from "react";

export type AdminSession = { admin: { id: string; email: string; nickname: string }; csrf_token: string; expires_at: string };
export function useAdmin() {
  const router = useRouter();
  const [session, setSession] = useState<AdminSession>();
  const [error, setError] = useState("");
  useEffect(() => { api.request<AdminSession>("/v1/admin/me").then(setSession).catch((reason: APIError) => { if (reason.code === "authentication_required" || reason.code === "session_expired") router.replace("/login"); else setError(reason.message); }); }, []);
  return { session, error };
}
