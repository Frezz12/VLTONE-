"use client";
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useState,
} from "react";
import Link from "next/link";
import { usePathname } from "next/navigation";
import {
  AudioLines,
  House,
  MessagesSquare,
  Bookmark,
  Bell,
  Search,
  Settings,
  LogOut,
  Sun,
  Moon,
  Users,
  ArrowUpRight,
} from "lucide-react";
import { api, type AccountSession } from "@vlt/api-client";
import { beginSignIn } from "@/lib/sso";
type Context = {
  session?: AccountSession;
  loading: boolean;
  locale: "ru" | "en";
  t: (ru: string, en: string) => string;
  setLocale: (v: "ru" | "en") => void;
  refresh: () => Promise<void>;
  login: () => void;
  error: string;
  notify: (s: string) => void;
};
const ForumContext = createContext<Context>(null!);
export const useForum = () => useContext(ForumContext);
export function ForumProvider({ children }: { children: React.ReactNode }) {
  const [session, setSession] = useState<AccountSession>();
  const [loading, setLoading] = useState(true);
  const [locale, setLocaleState] = useState<"ru" | "en">("ru");
  const [error, notify] = useState("");
  const t = useCallback(
    (ru: string, en: string) => (locale === "ru" ? ru : en),
    [locale],
  );
  const setLocale = (v: "ru" | "en") => {
    setLocaleState(v);
    localStorage.setItem("vlt-forum-locale", v);
    document.documentElement.lang = v;
  };
  const refresh = useCallback(async () => {
    try {
      setSession(await api.request<AccountSession>("/v1/me"));
    } catch {
      setSession(undefined);
    } finally {
      setLoading(false);
    }
  }, []);
  useEffect(() => {
    const saved = localStorage.getItem("vlt-forum-locale");
    if (saved === "en") setLocale("en");
    void refresh();
    const focus = () => {
      if (!document.hidden) void refresh();
    };
    window.addEventListener("focus", focus);
    document.addEventListener("visibilitychange", focus);
    return () => {
      window.removeEventListener("focus", focus);
      document.removeEventListener("visibilitychange", focus);
    };
  }, [refresh]);
  return (
    <ForumContext.Provider
      value={{
        session,
        loading,
        locale,
        t,
        setLocale,
        refresh,
        error,
        notify,
        login: () => {
          void beginSignIn(locale).catch((e) => notify(e.message));
        },
      }}
    >
      {children}
    </ForumContext.Provider>
  );
}
export function Shell({ children }: { children: React.ReactNode }) {
  const { session, t, locale, setLocale, login, refresh, error, notify } =
    useForum();
  const path = usePathname();
  const [dark, setDark] = useState(false);
  const [unread, setUnread] = useState(0);
  useEffect(() => {
    const saved = localStorage.getItem("vlt-forum-theme");
    const value = saved
      ? saved === "dark"
      : matchMedia("(prefers-color-scheme: dark)").matches;
    setDark(value);
    document.documentElement.dataset.theme = value ? "dark" : "light";
  }, []);
  useEffect(() => {
    if (!session) {
      setUnread(0);
      return;
    }
    const poll = () => {
      if (!document.hidden)
        void api
          .request<{ unread: number }>("/v1/forum/notifications?limit=1")
          .then((v) => setUnread(v.unread))
          .catch(() => {});
    };
    poll();
    const timer = setInterval(poll, 30000);
    window.addEventListener("focus", poll);
    return () => {
      clearInterval(timer);
      window.removeEventListener("focus", poll);
    };
  }, [session, path]);
  const nav = [
    { href: "/", label: t("Лента", "Feed"), icon: House },
    { href: "/forums", label: t("Форумы", "Forums"), icon: MessagesSquare },
    { href: "/saved", label: t("Сохранённое", "Saved"), icon: Bookmark },
    {
      href: "/notifications",
      label: t("Уведомления", "Notifications"),
      icon: Bell,
    },
  ];
  return (
    <>
      <a className="skip" href="#content">
        {t("К содержимому", "Skip to content")}
      </a>
      <header className="topbar">
        <Link href="/" className="brand">
          <span className="brand-icon">
            <AudioLines size={22} />
          </span>
          VLT<span className="brand-one">ONE</span>
          <span className="brand-forum">forum</span>
        </Link>
        <form action="/search" className="search">
          <Search size={17} />
          <input
            name="q"
            aria-label={t("Поиск", "Search")}
            placeholder={t(
              "Найти обсуждение, идею, модуль…",
              "Find a discussion, idea, module…",
            )}
          />
          <kbd>↵</kbd>
        </form>
        <div className="top-actions">
          <Link
            className="icon-button mobile-search-link"
            href="/search"
            aria-label={t("Поиск", "Search")}
          >
            <Search size={18} />
          </Link>
          <button
            className="icon-button"
            onClick={() => {
              const value = !dark;
              setDark(value);
              document.documentElement.dataset.theme = value ? "dark" : "light";
              localStorage.setItem("vlt-forum-theme", value ? "dark" : "light");
            }}
            aria-label={t("Сменить тему", "Change theme")}
          >
            {dark ? <Sun size={18} /> : <Moon size={18} />}
          </button>
          <button
            className="small"
            onClick={() => setLocale(locale === "ru" ? "en" : "ru")}
          >
            {locale.toUpperCase()}
          </button>
          {session ? (
            <Link
              className="avatar"
              href={`/u/${encodeURIComponent(session.user.nickname)}`}
              title={session.user.nickname}
            >
              {session.user.nickname.slice(0, 1).toUpperCase()}
            </Link>
          ) : (
            <button className="primary small" onClick={login}>
              {t("Войти", "Sign in")}
            </button>
          )}
        </div>
      </header>
      <div className="workspace">
        <aside className="sidebar">
          <div className="nav-label">{t("СООБЩЕСТВО", "COMMUNITY")}</div>
          <nav aria-label={t("Основная навигация", "Main navigation")}>
            {nav.map(({ href, label, icon: Icon }) => (
              <Link
                key={href}
                href={href}
                className={path === href ? "nav-item active" : "nav-item"}
              >
                <Icon size={19} />
                <span>{label}</span>
                {href === "/notifications" && unread > 0 && (
                  <span className="badge">{unread}</span>
                )}
              </Link>
            ))}
          </nav>
          <div className="sidebar-bottom">
            <Link className="nav-item" href="/settings">
              <Settings size={18} />
              <span>{t("Мой профиль", "My profile")}</span>
            </Link>
            {session && (
              <button
                className="nav-item"
                onClick={async () => {
                  try {
                    await api.json(
                      "/v1/web/auth/logout",
                      "POST",
                      {},
                      session.csrf_token,
                    );
                    await refresh();
                  } catch (e) {
                    notify((e as Error).message);
                  }
                }}
              >
                <LogOut size={18} />
                <span>{t("Выйти", "Sign out")}</span>
              </button>
            )}
            <a className="site-link" href="https://vltstudio.ru">
              VLTONE <ArrowUpRight size={15} />
            </a>
          </div>
        </aside>
        <main id="content" className="main" key={path}>
          {error && (
            <div role="alert" className="error">
              {error}
              <button onClick={() => notify("")}>×</button>
            </div>
          )}
          {children}
        </main>
        <aside className="rail">
          <div className="intro-card">
            <span className="eyebrow">VLTONE COMMUNITY</span>
            <AudioLines size={44} strokeWidth={1} />
            <h2>{t("Создаём звук. Вместе.", "Making sound. Together.")}</h2>
            <p>
              {t(
                "Делитесь музыкой, находите ответы и создавайте новые инструменты.",
                "Share your music, find answers and build new instruments.",
              )}
            </p>
          </div>
          <div className="rail-note">
            <Users size={18} />
            <h3>{t("Место для ваших идей", "A place for your ideas")}</h3>
            <p>
              {t(
                "От первого вопроса до собственного Mini-модуля. Здесь важен каждый опыт.",
                "From your first question to your own Mini module. Every experience matters here.",
              )}
            </p>
            <Link href="/sections/creator">Creator & Mini →</Link>
          </div>
          <div className="rail-note">
            <h3>{t("Из форума — в проект", "From forum to project")}</h3>
            <p>
              {t(
                "Скачивайте VLT-файлы в браузере программы. Они появятся в нужной библиотеке автоматически.",
                "Download VLT files in the app’s browser. They appear in the right library automatically.",
              )}
            </p>
          </div>
          <footer>
            © VLTONE · {t("Музыка объединяет", "Music connects")}
          </footer>
        </aside>
      </div>
    </>
  );
}
export function useResource<T>(path: string) {
  const [data, setData] = useState<T>();
  const [error, setError] = useState("");
  const [version, setVersion] = useState(0);
  useEffect(() => {
    const controller = new AbortController();
    setData(undefined);
    setError("");
    api
      .request<T>(path, { signal: controller.signal })
      .then(setData)
      .catch((e) => {
        if (!controller.signal.aborted) setError(e.message);
      });
    return () => controller.abort();
  }, [path, version]);
  return { data, error, reload: () => setVersion((v) => v + 1) };
}
export function State({
  error,
  loading,
  empty,
}: {
  error?: string;
  loading?: boolean;
  empty?: boolean;
}) {
  const { t } = useForum();
  if (error)
    return (
      <p className="error" role="alert">
        {error}
      </p>
    );
  if (loading)
    return (
      <div className="skeleton" role="status">
        {t("Загружаем…", "Loading…")}
      </div>
    );
  if (empty)
    return (
      <div className="empty">
        <MessagesSquare size={32} />
        <h3>{t("Здесь пока тихо", "It’s quiet here")}</h3>
        <p>
          {t(
            "Начните разговор — поделитесь идеей или задайте вопрос.",
            "Start a conversation — share an idea or ask a question.",
          )}
        </p>
      </div>
    );
  return null;
}
