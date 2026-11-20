// SPDX-License-Identifier: MIT

// The mock app: shell, three screens, two dialogs and the theme switch. The screen lives in the URL
// hash (#/dashboard, #/settings, #/controls) and the theme in ?theme=<id>, so a screenshot script
// can open any combination directly.
import { useEffect, useState } from "react";
import { Button } from "./components/Button";
import { Dialog } from "./components/Dialog";
import { Select } from "./components/Select";
import { TextField } from "./components/TextField";
import { AppBar } from "./components/shell/AppBar";
import { Sidebar, type NavGroup } from "./components/shell/Sidebar";
import { StatusBar } from "./components/shell/StatusBar";
import { Controls } from "./screens/Controls";
import { Dashboard } from "./screens/Dashboard";
import { Settings } from "./screens/Settings";

const themes = [
  { value: "instrument-light", label: "Light" },
  { value: "instrument-dark", label: "Dark" },
  { value: "instrument-high-contrast", label: "High contrast" },
];

const screens = ["dashboard", "settings", "controls"] as const;
type Screen = (typeof screens)[number];

const nav: NavGroup[] = [
  { heading: "Monitor", items: [{ id: "dashboard", label: "Dashboard", count: 2 }] },
  { heading: "Configure", items: [{ id: "settings", label: "Settings" }] },
  { heading: "Reference", items: [{ id: "controls", label: "Controls" }] },
];

function initialScreen(): Screen {
  const id = window.location.hash.replace("#/", "");
  return (screens as readonly string[]).includes(id) ? (id as Screen) : "dashboard";
}

function initialTheme(): string {
  const id = new URLSearchParams(window.location.search).get("theme") ?? "";
  return themes.some((t) => t.value === id) ? id : "instrument-light";
}

export function App() {
  const [screen, setScreen] = useState<Screen>(initialScreen);
  const [theme, setTheme] = useState(initialTheme);
  const [dialog, setDialog] = useState<"none" | "capture" | "reset">("none");
  const [channel, setChannel] = useState("bus-a/pressure");
  const [message, setMessage] = useState("Ready");

  useEffect(() => {
    document.documentElement.dataset.theme = theme;
  }, [theme]);

  // Back/forward and typed hashes change the screen without a reload.
  useEffect(() => {
    const onHash = () => setScreen(initialScreen());
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  const navigate = (id: string) => {
    setScreen(id as Screen);
    window.history.replaceState(null, "", `${window.location.search}#/${id}`);
  };

  const close = () => setDialog("none");

  return (
    <div className="tshell">
      <AppBar
        title="Telemetry Bench"
        crumb={screen}
        actions={
          <Select label="Theme" hideLabel compact value={theme} options={themes} onChange={setTheme} />
        }
      />
      <div className="tshell-middle">
        <Sidebar groups={nav} current={screen} onNavigate={navigate} />
        <main className="tshell-main">
          {screen === "dashboard" ? <Dashboard onNewCapture={() => setDialog("capture")} /> : null}
          {screen === "settings" ? (
            <Settings onReset={() => setDialog("reset")} onSaved={() => setMessage("Settings saved")} />
          ) : null}
          {screen === "controls" ? <Controls /> : null}
        </main>
      </div>
      <StatusBar
        items={[
          { text: "Connected 10.0.4.17", dot: "ok" },
          { text: "2 running · 1 stalled", dot: "warn", secondary: true },
          { text: message, end: true },
          { text: theme, end: true, secondary: true },
        ]}
      />

      <Dialog
        open={dialog === "capture"}
        title="New capture"
        onClose={close}
        actions={
          <>
            <Button variant="secondary" onClick={close}>
              Cancel
            </Button>
            <Button
              onClick={() => {
                setMessage(`Started ${channel}`);
                close();
              }}
            >
              Start capture
            </Button>
          </>
        }
      >
        <p>Records the channel into the ring buffer until you stop it or the buffer fills.</p>
        <TextField label="Channel" value={channel} onChange={setChannel} mono />
        <Select
          label="Duration"
          value="until-stop"
          onChange={() => undefined}
          options={[
            { value: "until-stop", label: "Until stopped" },
            { value: "60", label: "1 minute" },
          ]}
        />
      </Dialog>

      <Dialog
        open={dialog === "reset"}
        title="Reset bench?"
        onClose={close}
        actions={
          <>
            <Button variant="secondary" onClick={close}>
              Keep settings
            </Button>
            <Button
              variant="danger"
              onClick={() => {
                setMessage("Bench reset");
                close();
              }}
            >
              Reset bench
            </Button>
          </>
        }
      >
        <p>Channel maps and calibration for Bench 3 are cleared. Recorded captures are kept. This cannot be undone.</p>
      </Dialog>
    </div>
  );
}
