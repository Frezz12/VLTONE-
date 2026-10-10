export type AdminIdentity = { id: string; email: string; nickname: string; user_id?: string | null; is_owner: boolean; permissions: string[]; status?: string };
export const permissionSections = [
  ["dashboard", "Обзор", false], ["users", "Пользователи и доступ к приложению", true],
  ["bugs", "Баги", true], ["crashes", "Краши", false], ["releases", "Релизы", true],
  ["backgrounds", "Фоны браузера", true], ["models", "Модели AI", true],
  ["prompts", "Промпты", true], ["audit", "Аудит", false], ["tasks", "Доска задач", true],
] as const;
export function canAdmin(admin: AdminIdentity | undefined, permission: string) {
  return !!admin && (admin.is_owner || admin.permissions?.includes(permission));
}
export function pagePermission(path: string) {
  const section = path.split("/")[1];
  return ({ "": "dashboard.read", users: "users.read", bugs: "bugs.read", crashes: "crashes.read", releases: "releases.read", "browser-backgrounds": "backgrounds.read", models: "models.read", prompts: "prompts.read", audit: "audit.read", tasks: "tasks.read", team: "owner" } as Record<string, string>)[section];
}
export function canVisit(admin: AdminIdentity | undefined, path: string) {
  return canAdmin(admin, pagePermission(path));
}
export function adminHome(admin: AdminIdentity) {
  return ["/", "/tasks", "/users", "/bugs", "/crashes", "/releases", "/browser-backgrounds", "/models", "/prompts", "/audit", "/team"].find(path => canVisit(admin, path)) ?? "/tasks";
}
