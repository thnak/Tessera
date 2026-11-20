// SPDX-License-Identifier: MIT

// Settings: a real form with every input kind, inline validation, a dirty-aware save bar and a
// danger zone that confirms through the dialog.
import { useState, type FormEvent } from "react";
import { Button } from "../components/Button";
import { Checkbox } from "../components/Checkbox";
import { Panel } from "../components/Panel";
import { RadioGroup } from "../components/RadioGroup";
import { Select } from "../components/Select";
import { Slider } from "../components/Slider";
import { Switch } from "../components/Switch";
import { TextField } from "../components/TextField";
import "./page.css";
import "./settings.css";

type Form = {
  name: string;
  host: string;
  rate: string;
  buffer: number;
  trigger: string;
  autoStart: boolean;
  keepRaw: boolean;
  notify: boolean;
  telemetry: boolean;
};

const initial: Form = {
  name: "Bench 3",
  host: "10.0.4.17:5025",
  rate: "48000",
  buffer: 64,
  trigger: "edge",
  autoStart: true,
  keepRaw: false,
  notify: true,
  telemetry: false,
};

function hostError(host: string): string | undefined {
  if (host.trim() === "") return "Enter a host and port.";
  if (!/^[\w.-]+:\d{1,5}$/.test(host.trim())) return "Use host:port, for example 10.0.4.17:5025.";
  return undefined;
}

export function Settings({ onReset, onSaved }: { onReset: () => void; onSaved: () => void }) {
  const [saved, setSaved] = useState<Form>(initial);
  const [form, setForm] = useState<Form>(initial);
  const set = <K extends keyof Form>(key: K, value: Form[K]) => setForm({ ...form, [key]: value });

  const dirty = JSON.stringify(form) !== JSON.stringify(saved);
  const nameError = form.name.trim() === "" ? "A bench needs a name." : undefined;
  const hostErr = hostError(form.host);
  const valid = !nameError && !hostErr;

  const submit = (e: FormEvent) => {
    e.preventDefault();
    if (!valid) return;
    setSaved(form);
    onSaved();
  };

  return (
    <form className="page settings-page" onSubmit={submit} noValidate>
      <div className="page-header">
        <div className="page-heading">
          <span className="m-micro">Configure / Bench 3</span>
          <h1 className="page-title m-display">Settings</h1>
          <p className="page-subtitle">Capture defaults for this bench.</p>
        </div>
      </div>

      <div className="columns">
        <Panel index="01" title="Capture" screws>
          <TextField label="Bench name" value={form.name} onChange={(v) => set("name", v)} required error={nameError} />
          <TextField
            label="Instrument address"
            value={form.host}
            onChange={(v) => set("host", v)}
            mono
            hint="host:port of the SCPI endpoint"
            error={hostErr}
          />
          <Select
            label="Sample rate"
            value={form.rate}
            onChange={(v) => set("rate", v)}
            options={[
              { value: "1000", label: "1 kHz" },
              { value: "12000", label: "12 kHz" },
              { value: "48000", label: "48 kHz" },
              { value: "96000", label: "96 kHz" },
            ]}
            hint="Higher rates need a larger buffer."
          />
          <Slider label="Ring buffer" value={form.buffer} onChange={(v) => set("buffer", v)} min={8} max={256} step={8} unit=" MB" />
          <RadioGroup
            legend="Trigger"
            value={form.trigger}
            onChange={(v) => set("trigger", v)}
            inline
            options={[
              { value: "free", label: "Free run" },
              { value: "edge", label: "Edge" },
              { value: "level", label: "Level" },
              { value: "ext", label: "External", disabled: true },
            ]}
          />
        </Panel>

        <div className="stack">
          <Panel index="02" title="Behaviour">
            <Checkbox
              label="Start capture on connect"
              checked={form.autoStart}
              onChange={(v) => set("autoStart", v)}
              description="Resumes the last channel set."
            />
            <Checkbox label="Keep raw frames" checked={form.keepRaw} onChange={(v) => set("keepRaw", v)} />
            <Checkbox label="Write to network share" checked={false} onChange={() => undefined} disabled />
            <div className="divider" />
            <Switch label="Desktop notifications" checked={form.notify} onChange={(v) => set("notify", v)} />
            <Switch label="Send anonymous usage data" checked={form.telemetry} onChange={(v) => set("telemetry", v)} />
            <Switch label="Hardware timestamping" checked={false} onChange={() => undefined} disabled />
          </Panel>

          <Panel index="03" title="Danger zone">
            <div className="settings-danger">
              <span className="settings-hazard" aria-hidden="true" />
              <div className="settings-danger-text">
                <span>Reset bench</span>
                <span className="note">Clears channel maps and calibration. Captures are kept.</span>
              </div>
              <Button variant="danger" onClick={onReset}>
                Reset…
              </Button>
            </div>
          </Panel>
        </div>
      </div>

      <div className="settings-savebar m-inverse">
        <span className="settings-savestate m-micro">
          <span className={`m-led ${dirty ? "m-led-warn" : "m-led-ok"}`} aria-hidden="true" />
          {dirty ? "unsaved changes" : "all changes saved"}
        </span>
        <div className="page-actions">
          <Button variant="secondary" disabled={!dirty} onClick={() => setForm(saved)}>
            Revert
          </Button>
          <Button type="submit" disabled={!dirty || !valid}>
            Save
          </Button>
        </div>
      </div>
    </form>
  );
}
