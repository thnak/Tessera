// SPDX-License-Identifier: MIT

// Controls: every control in every state side by side, the type scale, and the colour tokens of the
// active theme (names read from instrument.theme.json, colours from the live CSS variables), so a
// reviewer can judge the language without hunting through screens.
import { useState } from "react";
import theme from "../../../../../packages/tessera-ui/themes/instrument.theme.json";
import { Badge } from "../components/Badge";
import { Button, type ButtonVariant } from "../components/Button";
import { Checkbox } from "../components/Checkbox";
import { Panel } from "../components/Panel";
import { RadioGroup } from "../components/RadioGroup";
import { Select } from "../components/Select";
import { Slider } from "../components/Slider";
import { Switch } from "../components/Switch";
import { TextField } from "../components/TextField";
import "./page.css";
import "./controls.css";

const variants: ButtonVariant[] = ["primary", "secondary", "ghost", "danger"];
const states = ["default", "hover", "active", "focus", "disabled"] as const;

const colorTokens = Object.entries(theme.themes[theme.start as keyof typeof theme.themes])
  .filter(([, v]) => v.startsWith("#") || v.startsWith("rgb"))
  .map(([k]) => k);

export function Controls() {
  const [text, setText] = useState("");
  const [on, setOn] = useState(true);
  const [level, setLevel] = useState(40);
  const [pick, setPick] = useState("a");
  return (
    <div className="page">
      <div className="page-header">
        <div className="page-heading">
          <h1 className="page-title">Controls</h1>
          <p className="page-subtitle">Every control and state on the active theme. Hover, active and focus are forced.</p>
        </div>
      </div>

      <Panel title="Button" meta="packages/tessera-ui/button.css">
        <div className="matrix" role="table" aria-label="Button states">
          <div className="matrix-row" role="row">
            <span className="matrix-head" role="columnheader">variant</span>
            {states.map((s) => (
              <span key={s} className="matrix-head" role="columnheader">
                {s}
              </span>
            ))}
          </div>
          {variants.map((v) => (
            <div key={v} className="matrix-row" role="row">
              <span className="matrix-name mono" role="rowheader">
                {v}
              </span>
              {states.map((s) => (
                <span key={s} className="matrix-cell" role="cell">
                  <Button
                    variant={v}
                    disabled={s === "disabled"}
                    forceState={s === "hover" || s === "active" || s === "focus" ? s : undefined}
                  >
                    Apply
                  </Button>
                </span>
              ))}
            </div>
          ))}
        </div>
        <div className="row">
          <span className="note mono">size sm</span>
          {variants.map((v) => (
            <Button key={v} variant={v} size="sm">
              Apply
            </Button>
          ))}
        </div>
      </Panel>

      <div className="columns">
        <Panel title="Text input">
          <TextField label="Default" value={text} onChange={setText} placeholder="Placeholder" />
          <TextField label="Focused" value="Bench 3" onChange={() => undefined} forceFocus />
          <TextField label="Invalid" value="10.0.4" onChange={() => undefined} error="Use host:port." mono />
          <TextField label="Disabled" value="read only" onChange={() => undefined} disabled />
          <Select label="Select" value="b" onChange={() => undefined} options={[{ value: "b", label: "48 kHz" }]} />
        </Panel>
        <Panel title="Selection">
          <Checkbox label="Checked" checked onChange={() => undefined} />
          <Checkbox label="Unchecked" checked={false} onChange={() => undefined} />
          <Checkbox label="Disabled checked" checked onChange={() => undefined} disabled />
          <Switch label="Switch" checked={on} onChange={setOn} />
          <Switch label="Switch disabled" checked onChange={() => undefined} disabled />
          <RadioGroup
            legend="Radio group"
            value={pick}
            onChange={setPick}
            inline
            options={[
              { value: "a", label: "One" },
              { value: "b", label: "Two" },
              { value: "c", label: "Off", disabled: true },
            ]}
          />
          <Slider label="Slider" value={level} onChange={setLevel} unit="%" />
          <Slider label="Slider disabled" value={70} onChange={() => undefined} unit="%" disabled />
        </Panel>
      </div>

      <div className="columns">
        <Panel title="Type">
          <span className="type-title">Title 18/24 IBM Plex Sans</span>
          <span>Body 14/20 IBM Plex Sans, the quick brown fox</span>
          <span className="label">Label 11/16 uppercase</span>
          <span className="mono">Mono 0123456789 48.20 MB/s</span>
          <div className="row">
            {(["neutral", "accent", "success", "warning", "danger"] as const).map((t) => (
              <Badge key={t} tone={t}>
                {t}
              </Badge>
            ))}
          </div>
        </Panel>
        <Panel title="Colour tokens" meta={`${colorTokens.length} tokens`}>
          <div className="swatches">
            {colorTokens.map((name) => (
              <div key={name} className="swatch">
                <span className="swatch-chip" style={{ backgroundColor: `var(${name})` }} />
                <span className="swatch-name mono">{name}</span>
              </div>
            ))}
          </div>
        </Panel>
      </div>
    </div>
  );
}
