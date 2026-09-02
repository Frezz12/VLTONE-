"use client";

type CollaborationAccessSwitchProps = {
  enabled: boolean;
  pending: boolean;
  label: string;
  error?: string;
  success?: string;
  onChange: (enabled: boolean) => void;
};

export function CollaborationAccessSwitch({ enabled, pending, label, error, success, onChange }: CollaborationAccessSwitchProps) {
  return <div className="collaboration-access">
    <div className="collaboration-access-row">
      <button
        type="button"
        className="collaboration-access-switch"
        role="switch"
        aria-checked={enabled}
        aria-busy={pending}
        aria-label={label}
        disabled={pending}
        onClick={() => onChange(!enabled)}
      >
        <span className="collaboration-access-track" aria-hidden="true"><span /></span>
      </button>
      <span className="collaboration-access-state">{pending ? "Сохраняем…" : enabled ? "Включён" : "Выключен"}</span>
    </div>
    {error && <span className="collaboration-access-error" role="alert">{error}</span>}
    {success !== undefined && <span className="sr-only" role="status" aria-live="polite">{success}</span>}
  </div>;
}
