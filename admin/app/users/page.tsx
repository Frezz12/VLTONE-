import { UserRegistry } from "@/components/user-registry";
import { Suspense } from "react";
export default function Users() { return <Suspense fallback={<p className="vlt-muted">Загрузка пользователей…</p>}><UserRegistry /></Suspense>; }
