"use client";
import { useEffect, useState } from "react";
import { api, type ForumProfile } from "@vlt/api-client";
import { useForum, useResource, State } from "./shell";
import { uploadFile } from "@/lib/upload";
import { attachmentURL } from "./content";
export function ProfileSettings() {
  const { t, session, loading, login } = useForum();
  const { data, error } = useResource<{ profile: ForumProfile }>(
    "/v1/forum/me/profile",
  );
  const [profile, setProfile] = useState<ForumProfile>();
  const [message, setMessage] = useState("");
  const [failure, setFailure] = useState("");
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(0);
  useEffect(() => {
    if (data) setProfile({ ...data.profile, links: data.profile.links ?? [] });
  }, [data]);
  if (!loading && !session)
    return (
      <button className="primary" onClick={login}>
        {t("Войти", "Sign in")}
      </button>
    );
  return (
    <>
      <div className="page-heading">
        <h1>{t("Мой профиль", "My profile")}</h1>
      </div>
      <State error={error} loading={!profile && !error} />
      {profile && (
        <form
          className="settings-form"
          onSubmit={async (e) => {
            e.preventDefault();
            if (!session) return;
            setBusy(true);
            setFailure("");
            try {
              await api.json(
                "/v1/forum/me/profile",
                "PUT",
                {
                  about: profile.about,
                  location: profile.location,
                  interests: profile.interests,
                  signature: profile.signature,
                  links: profile.links,
                  avatar_id: profile.avatar_id,
                  profile_completed: true,
                },
                session.csrf_token,
              );
              setMessage(t("Профиль сохранён.", "Profile saved."));
            } catch (e) {
              setFailure((e as Error).message);
            } finally {
              setBusy(false);
            }
          }}
        >
          <label>
            {t("Аватар", "Avatar")}
            {profile.avatar_id && (
              <img
                className="avatar large"
                src={attachmentURL(profile.avatar_id, "original")}
                alt=""
              />
            )}
            <input
              type="file"
              accept=".png,.jpg,.jpeg,.webp"
              disabled={busy}
              onChange={async (e) => {
                const file = e.target.files?.[0];
                if (!file || !session) return;
                if (file.size > 10000000) {
                  setFailure(
                    t(
                      "Изображение должно быть меньше 10 МБ.",
                      "Image must be smaller than 10 MB.",
                    ),
                  );
                  return;
                }
                setBusy(true);
                setFailure("");
                try {
                  const a = await uploadFile(
                    file,
                    session.csrf_token,
                    new AbortController().signal,
                    setProgress,
                    () => {},
                  );
                  setProfile({ ...profile, avatar_id: a.id });
                } catch (e) {
                  setFailure((e as Error).message);
                } finally {
                  setBusy(false);
                }
              }}
            />
            {busy && progress > 0 && <progress max={100} value={progress} />}
          </label>
          <label>
            {t("О себе", "About")}
            <textarea
              rows={5}
              maxLength={5000}
              value={profile.about}
              onChange={(e) =>
                setProfile({ ...profile, about: e.target.value })
              }
            />
          </label>
          <label>
            {t("Город", "Location")}
            <input
              maxLength={200}
              value={profile.location}
              onChange={(e) =>
                setProfile({ ...profile, location: e.target.value })
              }
            />
          </label>
          <label>
            {t("Интересы", "Interests")}
            <input
              maxLength={2000}
              value={profile.interests}
              onChange={(e) =>
                setProfile({ ...profile, interests: e.target.value })
              }
            />
          </label>
          <label>
            {t("Подпись", "Signature")}
            <input
              maxLength={1000}
              value={profile.signature}
              onChange={(e) =>
                setProfile({ ...profile, signature: e.target.value })
              }
            />
          </label>
          <fieldset>
            <legend>{t("Ссылки", "Links")}</legend>
            {profile.links.map((link, i) => (
              <div className="link-fields" key={i}>
                <input
                  aria-label={t("Название ссылки", "Link label")}
                  value={link.label}
                  maxLength={80}
                  onChange={(e) =>
                    setProfile({
                      ...profile,
                      links: profile.links.map((l, n) =>
                        n === i ? { ...l, label: e.target.value } : l,
                      ),
                    })
                  }
                />
                <input
                  type="url"
                  required
                  aria-label="URL"
                  value={link.url}
                  maxLength={500}
                  onChange={(e) =>
                    setProfile({
                      ...profile,
                      links: profile.links.map((l, n) =>
                        n === i ? { ...l, url: e.target.value } : l,
                      ),
                    })
                  }
                />
                <button
                  type="button"
                  onClick={() =>
                    setProfile({
                      ...profile,
                      links: profile.links.filter((_, n) => n !== i),
                    })
                  }
                >
                  ×
                </button>
              </div>
            ))}
            <button
              type="button"
              disabled={profile.links.length >= 8}
              onClick={() =>
                setProfile({
                  ...profile,
                  links: [...profile.links, { label: "", url: "https://" }],
                })
              }
            >
              {t("Добавить ссылку", "Add link")}
            </button>
          </fieldset>
          {failure && (
            <p className="error" role="alert">
              {failure}
            </p>
          )}
          {message && <p role="status">{message}</p>}
          <button className="primary" disabled={busy}>
            {t("Сохранить профиль", "Save profile")}
          </button>
        </form>
      )}
    </>
  );
}
