// SPDX-License-Identifier: MIT

// Notifications (issue #559): the toast stack and the bell with its history popover. The queue, expiry,
// dedupe, history cap and do-not-disturb are `workbench/notifications.js`; this draws them.
//
// The stack is one `aria-live="polite"` region that is always in the document (a live region must exist before
// its content changes to be announced), so a toast is announced once, politely, whichever region of the page
// produced it. An action button runs its command through the registry and dismisses the toast.

import { useEffect, useMemo, useRef, useSyncExternalStore } from "react";
import type { NotificationEntry } from "../workbench/index.js";
import type { Workbench } from "../page-workbench";

function useNotificationView(workbench: Workbench) {
  const { notifications } = workbench;
  const version = useSyncExternalStore(notifications.subscribe, notifications.version, notifications.version);
  return useMemo(() => notifications.snapshot(), [notifications, version]);
}

const SEVERITY_LABEL = { info: "Info", warning: "Warning", error: "Error" } as const;

function timeText(at: number): string {
  return new Date(at).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
}

function Actions({ entry, workbench, onRan }: { entry: NotificationEntry; workbench: Workbench; onRan?: () => void }) {
  return (
    <>
      {entry.actions.map((action) => (
        <button
          key={`${action.command}:${action.label}`}
          type="button"
          className="toast-action"
          data-command={action.command}
          onClick={() => {
            void workbench.run(action.command, action.args);
            onRan?.();
          }}
        >
          {action.label}
        </button>
      ))}
    </>
  );
}

/** The toast stack: bottom right, at most three at once, the rest queued behind them. */
export function ToastStack({ workbench }: { workbench: Workbench }) {
  const view = useNotificationView(workbench);
  const { notifications } = workbench;
  const hasToasts = view.toasts.length > 0;
  useEffect(() => {
    if (!hasToasts) {
      return;
    }
    const timer = window.setInterval(() => notifications.tick(), 1000);
    return () => window.clearInterval(timer);
  }, [hasToasts, notifications]);
  return (
    <div className="toast-stack" id="notifications" role="region" aria-label="Notifications">
      <div className="toast-live" id="notification-live" aria-live="polite" aria-atomic="false" aria-relevant="additions text">
        {view.toasts.map((toast) => (
          <div key={toast.id} className={`toast toast-${toast.severity}`} data-id={toast.id}>
            <span className="toast-severity visually-hidden">{SEVERITY_LABEL[toast.severity]}: </span>
            <span className="toast-message">{toast.message}</span>
            {toast.count > 1 ? (
              <span className="toast-count" aria-label={`${toast.count} times`}>
                {toast.count}
              </span>
            ) : null}
            <Actions entry={toast} workbench={workbench} onRan={() => notifications.dismiss(toast.id)} />
            <button type="button" className="toast-dismiss" aria-label="Dismiss notification" onClick={() => notifications.dismiss(toast.id)}>
              {"×"}
            </button>
          </div>
        ))}
        {view.queued > 0 ? <div className="toast-more">{`+${view.queued} more in the notification history`}</div> : null}
      </div>
    </div>
  );
}

/** The bell button (a toolbar widget) and its history popover. */
export function NotificationBell({ workbench, tabIndex }: { workbench: Workbench; tabIndex: number }) {
  const view = useNotificationView(workbench);
  const { notifications, centerOpen, setCenterOpen } = workbench;
  const button = useRef<HTMLButtonElement>(null);
  const panel = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!centerOpen) {
      return;
    }
    notifications.markRead();
    const onPointerDown = (event: PointerEvent) => {
      if (!panel.current?.contains(event.target as Node) && !button.current?.contains(event.target as Node)) {
        setCenterOpen(false);
      }
    };
    document.addEventListener("pointerdown", onPointerDown, true);
    panel.current?.querySelector<HTMLElement>("button, [tabindex]")?.focus();
    return () => document.removeEventListener("pointerdown", onPointerDown, true);
  }, [centerOpen, notifications, setCenterOpen]);

  const label = view.unread > 0 ? `Notifications, ${view.unread} unread` : view.dnd ? "Notifications, do not disturb" : "Notifications";
  return (
    <div className="bell-wrap">
      <button
        type="button"
        id="notifications-bell"
        ref={button}
        data-roving=""
        tabIndex={tabIndex}
        className="tb-button bell"
        aria-haspopup="dialog"
        aria-expanded={centerOpen}
        aria-controls={centerOpen ? "notification-center" : undefined}
        aria-label={label}
        title={label}
        onClick={() => setCenterOpen(!centerOpen)}
      >
        <span aria-hidden="true">{view.dnd ? "🔕︎" : "🔔︎"}</span>
        {view.unread > 0 ? (
          <span className="tb-badge" aria-hidden="true">
            {view.unread}
          </span>
        ) : null}
      </button>
      {centerOpen ? (
        <div
          className="notification-center"
          id="notification-center"
          role="dialog"
          aria-label="Notification history"
          ref={panel}
          onKeyDown={(event) => {
            if (event.key === "Escape") {
              event.preventDefault();
              event.stopPropagation();
              setCenterOpen(false);
              button.current?.focus();
            }
          }}
        >
          <div className="nc-head">
            <strong>Notifications</strong>
            <span className="nc-spacer" />
            <button type="button" id="notifications-dnd" aria-pressed={view.dnd} onClick={() => notifications.setDoNotDisturb(!view.dnd)}>
              Do not disturb
            </button>
            <button type="button" id="notifications-clear" disabled={view.history.length === 0} onClick={() => notifications.clearHistory()}>
              Clear all
            </button>
          </div>
          {view.history.length === 0 ? (
            <p className="nc-empty">No notifications</p>
          ) : (
            <ul className="nc-list" aria-label="Notification history">
              {view.history.map((entry) => (
                <li key={entry.id} className={`nc-item toast-${entry.severity}`}>
                  <span className="visually-hidden">{SEVERITY_LABEL[entry.severity]}: </span>
                  <span className="nc-message">{entry.message}</span>
                  {entry.count > 1 ? <span className="toast-count">{entry.count}</span> : null}
                  <time className="nc-time">{timeText(entry.at)}</time>
                  <Actions entry={entry} workbench={workbench} />
                </li>
              ))}
            </ul>
          )}
        </div>
      ) : null}
    </div>
  );
}
