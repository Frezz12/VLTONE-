"use client";
import { useEffect, useState } from "react";
import Link from "next/link";
import { useRouter } from "next/navigation";
import {
  Plus,
  Pin,
  Lock,
  MessageSquare,
  ArrowRight,
  Check,
} from "lucide-react";
import {
  api,
  type ForumArticle,
  type ForumTopic,
  type ForumSection,
  type ForumReply,
  type ForumProfile,
  type ForumNotification,
} from "@vlt/api-client";
import { useForum, useResource, State } from "./shell";
import { ContentCard, RichText } from "./content";
import { Composer } from "./composer";
import { ProfileSettings } from "./profile-settings";
export function ForumPage({ path }: { path: string[] }) {
  switch (path[0]) {
    case undefined:
      return <Feed />;
    case "forums":
      return <Forums />;
    case "sections":
      return <Section slug={path[1] ?? "general"} />;
    case "topics":
      return <Detail kind="topic" id={path[1] ?? ""} />;
    case "posts":
      return <Detail kind="article" id={path[1] ?? ""} />;
    case "u":
      return <Profile nickname={decodeURIComponent(path[1] ?? "")} />;
    case "saved":
      return <Saved />;
    case "notifications":
      return <Notifications />;
    case "settings":
      return <ProfileSettings />;
    case "search":
      return <Search />;
    default:
      return <State error="404 — Page not found" />;
  }
}
function Heading({
  title,
  subtitle,
  children,
}: {
  title: string;
  subtitle?: string;
  children?: React.ReactNode;
}) {
  return (
    <div className="page-heading">
      <div>
        <h1>{title}</h1>
        {subtitle && <p>{subtitle}</p>}
      </div>
      {children}
    </div>
  );
}
function Feed() {
  const { t, session } = useForum();
  const [following, setFollowing] = useState(false);
  const [rows, setRows] = useState<ForumArticle[]>([]);
  const [cursor, setCursor] = useState("");
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState("");
  const [version, setVersion] = useState(0);
  useEffect(() => {
    const abort = new AbortController();
    setLoading(true);
    setRows([]);
    setError("");
    api
      .request<{ articles: ForumArticle[]; next_cursor: string }>(
        `/v1/forum/feed?limit=20&following=${following}`,
        { signal: abort.signal },
      )
      .then((r) => {
        setRows(r.articles);
        setCursor(r.next_cursor);
      })
      .catch((e) => {
        if (!abort.signal.aborted) setError(e.message);
      })
      .finally(() => {
        if (!abort.signal.aborted) setLoading(false);
      });
    return () => abort.abort();
  }, [following, version, session?.user.id]);
  async function more() {
    setLoading(true);
    try {
      const r = await api.request<{
        articles: ForumArticle[];
        next_cursor: string;
      }>(
        `/v1/forum/feed?limit=20&following=${following}&cursor=${encodeURIComponent(cursor)}`,
      );
      setRows((old) => [
        ...old,
        ...r.articles.filter((a) => !old.some((b) => a.id === b.id)),
      ]);
      setCursor(r.next_cursor);
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setLoading(false);
    }
  }
  return (
    <>
      <Heading
        title={t("Лента сообщества", "Community feed")}
        subtitle={t(
          "Музыка, идеи и люди за ними.",
          "Music, ideas and the people behind them.",
        )}
      />
      <div className="tabs">
        <button
          className={!following ? "selected" : ""}
          onClick={() => setFollowing(false)}
        >
          {t("Все публикации", "Discover")}
        </button>
        <button
          className={following ? "selected" : ""}
          onClick={() => setFollowing(true)}
        >
          {t("Подписки", "Following")}
        </button>
      </div>
      <Composer onSaved={() => setVersion((v) => v + 1)} />
      <State
        error={error}
        loading={loading && !rows.length}
        empty={!loading && !error && !rows.length}
      />
      {rows.map((a) => (
        <ContentCard
          key={a.id}
          content={a}
          replyCount={a.comment_count}
          onChange={() => setVersion((v) => v + 1)}
        />
      ))}
      {cursor && (
        <button
          className="load-more"
          disabled={loading}
          onClick={() => void more()}
        >
          {t("Показать ещё", "Load more")}
        </button>
      )}
    </>
  );
}
function Forums() {
  const { t } = useForum();
  const { data, error } = useResource<{ sections: ForumSection[] }>(
    "/v1/forum/sections",
  );
  return (
    <>
      <Heading
        title={t("Форумы", "Forums")}
        subtitle={t(
          "Задавайте вопросы. Делитесь открытиями.",
          "Ask questions. Share discoveries.",
        )}
      />
      <State error={error} loading={!data && !error} />
      <div className="section-list">
        {data?.sections.map((s) => (
          <Link
            className="section-card"
            href={`/sections/${s.slug}`}
            key={s.id}
          >
            <span className="section-icon">
              <MessageSquare size={23} />
            </span>
            <div>
              <h2>{t(s.title_ru, s.title_en)}</h2>
              <p>{t(s.description_ru, s.description_en)}</p>
              <small>
                {s.topic_count} {t("тем", "topics")}
              </small>
            </div>
            <ArrowRight className="push" size={19} />
          </Link>
        ))}
      </div>
    </>
  );
}
function TopicList({ topics }: { topics: ForumTopic[] }) {
  const { t } = useForum();
  return (
    <div className="topic-list">
      {topics.map((topic) => (
        <Link className="topic-row" href={`/topics/${topic.id}`} key={topic.id}>
          <div className="topic-symbol">
            {topic.accepted_post_id ? (
              <Check size={20} />
            ) : (
              <MessageSquare size={20} />
            )}
          </div>
          <div>
            <h3>
              {topic.pinned && <Pin size={14} />}{" "}
              {topic.locked && <Lock size={14} />} {topic.title}
            </h3>
            <p>
              {topic.author.nickname} · {topic.reply_count}{" "}
              {t("ответов", "replies")}
            </p>
          </div>
          <ArrowRight className="push" size={16} />
        </Link>
      ))}
    </div>
  );
}
function Section({ slug }: { slug: string }) {
  const { t, session, login } = useForum();
  const router = useRouter();
  const [create, setCreate] = useState(false);
  const [offset, setOffset] = useState(0);
  const { data, error } = useResource<{
    section: ForumSection;
    topics: ForumTopic[];
    total: number;
  }>(
    `/v1/forum/sections/${encodeURIComponent(slug)}/topics?limit=30&offset=${offset}`,
  );
  return (
    <>
      <Link className="breadcrumb" href="/forums">
        ← {t("Форумы", "Forums")}
      </Link>
      <Heading
        title={
          data
            ? t(data.section.title_ru, data.section.title_en)
            : t("Обсуждения", "Discussions")
        }
        subtitle={
          data
            ? t(data.section.description_ru, data.section.description_en)
            : ""
        }
      >
        <button
          className="primary"
          onClick={() => (session ? setCreate(!create) : login())}
        >
          <Plus size={16} />
          {t("Новая тема", "New topic")}
        </button>
      </Heading>
      {create && (
        <Composer
          kind="topic"
          section={data?.section.id}
          onSaved={(id) => router.push(`/topics/${id}`)}
          onCancel={() => setCreate(false)}
        />
      )}
      <State
        error={error}
        loading={!data && !error}
        empty={!!data && !data.topics.length}
      />
      {data && (
        <>
          <TopicList topics={data.topics} />
          <Pages
            offset={offset}
            total={data.total}
            size={30}
            setOffset={setOffset}
          />
        </>
      )}
    </>
  );
}
function Pages({
  offset,
  total,
  size,
  setOffset,
}: {
  offset: number;
  total: number;
  size: number;
  setOffset: (n: number) => void;
}) {
  const { t } = useForum();
  return total > size ? (
    <div className="pager">
      <button
        disabled={!offset}
        onClick={() => setOffset(Math.max(0, offset - size))}
      >
        {t("Назад", "Previous")}
      </button>
      <span>
        {Math.floor(offset / size) + 1} / {Math.ceil(total / size)}
      </span>
      <button
        disabled={offset + size >= total}
        onClick={() => setOffset(offset + size)}
      >
        {t("Далее", "Next")}
      </button>
    </div>
  ) : null;
}
function Detail({ kind, id }: { kind: "article" | "topic"; id: string }) {
  const { t, session, notify } = useForum();
  const router = useRouter();
  const [version, setVersion] = useState(0);
  const [offset, setOffset] = useState(0);
  const [quote, setQuote] = useState("");
  const [parentComment, setParentComment] = useState<string>();
  const resource = useResource<{ article?: ForumArticle; topic?: ForumTopic }>(
    `/v1/forum/${kind === "article" ? "articles" : "topics"}/${id}?v=${version}`,
  );
  const replies = useResource<{
    posts?: ForumReply[];
    comments?: ForumReply[];
    total: number;
  }>(
    `/v1/forum/${kind === "article" ? "articles" : "topics"}/${id}/${kind === "article" ? "comments" : "posts"}?limit=30&offset=${offset}&v=${version}`,
  );
  const content = resource.data?.[kind];
  const topic = resource.data?.topic;
  const rows = replies.data?.posts ?? replies.data?.comments ?? [];
  const refresh = () => setVersion((v) => v + 1);
  return (
    <>
      <button className="breadcrumb" onClick={() => router.back()}>
        ← {t("Назад", "Back")}
      </button>
      <State error={resource.error} loading={!content && !resource.error} />
      {content && (
        <ContentCard
          content={content}
          kind={kind}
          detail
          replyCount={
            kind === "article"
              ? (content as ForumArticle).comment_count
              : (content as ForumTopic).reply_count
          }
          onChange={refresh}
        />
      )}
      <h2 className="discussion-title">
        {t("Обсуждение", "Discussion")} <span>{replies.data?.total ?? 0}</span>
      </h2>
      <State error={replies.error} />
      {rows.map((row) => (
        <ContentCard
          key={row.id}
          content={row}
          kind={kind === "article" ? "comment" : "post"}
          detail
          onChange={refresh}
          accepted={topic?.accepted_post_id === row.id}
          onAccept={
            kind === "topic" &&
            topic?.section?.slug === "help" &&
            session?.user.id === content?.author.id
              ? () => {
                  void api
                    .json(
                      `/v1/forum/topics/${id}/answer`,
                      "PUT",
                      {
                        post_id:
                          topic?.accepted_post_id === row.id ? null : row.id,
                      },
                      session!.csrf_token,
                    )
                    .then(refresh)
                    .catch((e) => notify(e.message));
                }
              : undefined
          }
          onQuote={(text, replyID) => {
            setQuote(`> ${text.split("\n").join("\n> ")}\n\n`);
            if (kind === "article") setParentComment(replyID);
            document
              .getElementById("reply-composer")
              ?.scrollIntoView({ block: "center" });
          }}
        />
      ))}
      {replies.data && (
        <Pages
          offset={offset}
          total={replies.data.total}
          size={30}
          setOffset={setOffset}
        />
      )}
      <div id="reply-composer">
        {topic?.locked ? (
          <div className="empty">
            <Lock size={20} />
            {t(
              "Тема закрыта для новых ответов.",
              "This topic is closed to new replies.",
            )}
          </div>
        ) : (
          content && (
            <Composer
              kind={kind === "article" ? "comment" : "post"}
              parent={id}
              quote={quote}
              parentComment={parentComment}
              onSaved={() => {
                setQuote("");
                setParentComment(undefined);
                refresh();
              }}
            />
          )
        )}
      </div>
    </>
  );
}
function Profile({ nickname }: { nickname: string }) {
  const { t, session, login, notify } = useForum();
  const {
    data: response,
    error,
    reload,
  } = useResource<{
    profile: {
      user: { id: string; nickname: string; created_at: string };
      profile: ForumProfile;
      recent_articles: ForumArticle[];
      recent_topics: ForumTopic[];
    };
  }>(`/v1/forum/users/${encodeURIComponent(nickname)}`);
  const data = response?.profile;
  const social = useResource<{
    followers: number;
    following: number;
    followed: boolean;
  }>(
    `/v1/forum/users/${data?.user.id ?? "00000000-0000-0000-0000-000000000000"}/social`,
  );
  return (
    <>
      <State error={error} loading={!data && !error} />
      {data && (
        <>
          <div className="profile-cover">
            <span>VLTONE COMMUNITY</span>
          </div>
          <div className="profile-header">
            <span className="avatar large">
              {data.profile.avatar_id ? (
                <img
                  src={`/api/v1/forum/attachments/${data.profile.avatar_id}/media/original`}
                  alt=""
                />
              ) : (
                nickname.slice(0, 1)
              )}
            </span>
            <Heading title={nickname} subtitle={data.profile.location}>
              {session?.user.id === data.user.id ? (
                <Link href="/settings" className="secondary">
                  {t("Редактировать", "Edit profile")}
                </Link>
              ) : (
                <button
                  className="primary"
                  onClick={() => {
                    if (!session) {
                      login();
                      return;
                    }
                    void api
                      .json(
                        `/v1/forum/users/${data.user.id}/follow`,
                        social.data?.followed ? "DELETE" : "PUT",
                        {},
                        session.csrf_token,
                      )
                      .then(social.reload)
                      .catch((e) => notify(e.message));
                  }}
                >
                  {social.data?.followed
                    ? t("Вы подписаны", "Following")
                    : t("Подписаться", "Follow")}
                </button>
              )}
            </Heading>
            <RichText text={data.profile.about} />
            <p className="hint">
              {social.data?.followers ?? 0} {t("подписчиков", "followers")} ·{" "}
              {social.data?.following ?? 0} {t("подписок", "following")}
            </p>
            {data.profile.links?.map((link) => (
              <a
                className="profile-link"
                key={link.url}
                href={/^https?:\/\//.test(link.url) ? link.url : undefined}
                rel="nofollow noopener noreferrer"
                target="_blank"
              >
                {link.label} ↗
              </a>
            ))}
          </div>
          <h2 className="discussion-title">{t("Публикации", "Posts")}</h2>
          {data.recent_articles?.map((a) => (
            <ContentCard
              content={a}
              key={a.id}
              replyCount={a.comment_count}
              onChange={reload}
            />
          ))}
          <TopicList topics={data.recent_topics ?? []} />
        </>
      )}
    </>
  );
}
function Saved() {
  const { t, session, loading, login } = useForum();
  const [offset, setOffset] = useState(0);
  const { data, error, reload } = useResource<{
    articles: ForumArticle[];
    topics: ForumTopic[];
    has_more: boolean;
  }>(`/v1/forum/bookmarks?limit=30&offset=${offset}`);
  if (!loading && !session)
    return (
      <button className="primary" onClick={login}>
        {t("Войти, чтобы открыть сохранённое", "Sign in to see saved posts")}
      </button>
    );
  return (
    <>
      <Heading title={t("Сохранённое", "Saved")} />
      <State
        error={error}
        loading={!data && !error}
        empty={!!data && !data.articles.length && !data.topics.length}
      />
      {data?.articles.map((a) => (
        <ContentCard
          content={a}
          key={a.id}
          replyCount={a.comment_count}
          onChange={reload}
        />
      ))}
      <TopicList topics={data?.topics ?? []} />
      {(offset > 0 || data?.has_more) && (
        <Pages
          offset={offset}
          total={offset + 30 + (data?.has_more ? 1 : 0)}
          size={30}
          setOffset={setOffset}
        />
      )}
    </>
  );
}
function Notifications() {
  const { t, session, notify } = useForum();
  const [offset, setOffset] = useState(0);
  const { data, error, reload } = useResource<{
    notifications: ForumNotification[];
    unread: number;
  }>(`/v1/forum/notifications?limit=30&offset=${offset}`);
  useEffect(() => {
    const timer = setInterval(() => {
      if (!document.hidden) reload();
    }, 30000);
    return () => clearInterval(timer);
  }, [offset]);
  return (
    <>
      <Heading title={t("Уведомления", "Notifications")}>
        <button
          onClick={() => {
            if (session)
              void api
                .json(
                  "/v1/forum/notifications/read",
                  "PUT",
                  {},
                  session.csrf_token,
                )
                .then(reload)
                .catch((e) => notify(e.message));
          }}
        >
          {t("Прочитать все", "Mark all read")}
        </button>
      </Heading>
      <State
        error={error}
        loading={!data && !error}
        empty={!!data && !data.notifications?.length}
      />
      {data?.notifications?.map((n) => (
        <Link
          className={`notification ${!n.read_at ? "unread" : ""}`}
          key={n.id}
          href={
            n.target_type === "topic"
              ? `/topics/${n.target_id}`
              : n.target_type === "article"
                ? `/posts/${n.target_id}`
                : `/u/${encodeURIComponent(n.actor.nickname)}`
          }
        >
          <span className="notification-dot" />
          <div>
            <strong>
              {n.kind === "follow"
                ? t("Новый подписчик", "New follower")
                : n.kind === "reply"
                  ? t("Новый ответ в вашей теме", "New reply to your topic")
                  : t("Новый комментарий", "New comment")}
            </strong>
            <time>{new Date(n.created_at).toLocaleString()}</time>
          </div>
          <ArrowRight size={16} className="push" />
        </Link>
      ))}
      {(offset > 0 || data?.notifications?.length === 30) && (
        <Pages
          offset={offset}
          total={offset + 30 + (data?.notifications?.length === 30 ? 1 : 0)}
          size={30}
          setOffset={setOffset}
        />
      )}
    </>
  );
}
function Search() {
  const { t } = useForum();
  const [q, setQ] = useState("");
  const [offset, setOffset] = useState(0);
  useEffect(
    () => setQ(new URLSearchParams(location.search).get("q") ?? ""),
    [],
  );
  const { data, error } = useResource<{
    articles: ForumArticle[];
    topics: ForumTopic[];
  }>(`/v1/forum/search?q=${encodeURIComponent(q)}&limit=30&offset=${offset}`);
  return (
    <>
      <Heading title={t("Поиск", "Search")} subtitle={q} />
      <form action="/search" className="mobile-search">
        <input name="q" defaultValue={q} aria-label={t("Поиск", "Search")} />
        <button>{t("Найти", "Search")}</button>
      </form>
      <State error={error} loading={!data && !error} />
      <TopicList topics={data?.topics ?? []} />
      {data?.articles.map((a) => (
        <ContentCard key={a.id} content={a} replyCount={a.comment_count} />
      ))}
      {data && <State empty={!data.articles.length && !data.topics.length} />}
      <Pages
        offset={offset}
        total={
          offset +
          30 +
          (data?.articles.length === 30 || data?.topics.length === 30 ? 1 : 0)
        }
        size={30}
        setOffset={setOffset}
      />
    </>
  );
}
