// SPDX-License-Identifier: MIT

// Controls: every control in every state side by side, the type scale, and the colour tokens of the
// active theme (names read from instrument.theme.json, colours from the live CSS variables), so a
// reviewer can judge the language without hunting through screens.
import { useState } from "react";
import theme from "../../../../../packages/tessera-ui/themes/instrument.theme.json";
import material from "../../tokens/material.tokens.json";
import { Badge } from "../components/Badge";
import { Button, type ButtonVariant } from "../components/Button";
import { Checkbox } from "../components/Checkbox";
import { Knob } from "../components/Knob";
import { Meter } from "../components/Meter";
import { Segmented } from "../components/Segmented";
import { Panel } from "../components/Panel";
import { RadioGroup } from "../components/RadioGroup";
import { Select } from "../components/Select";
import { Slider } from "../components/Slider";
import { Switch } from "../components/Switch";
import { TextField } from "../components/TextField";
import "./page.css";
import "./controls.css";

const variants: ButtonVariant[] = ["primary", "secondary", "ghost", "danger", "dark"];
const states = ["default", "hover", "active", "focus", "disabled"] as const;

const isColor = ([, v]: [string, string]) => v.startsWith("#") || v.startsWith("rgb");
const colorTokens = Object.entries(theme.themes[theme.start as keyof typeof theme.themes]).filter(isColor).map(([k]) => k);
// The material swatches skip the purely decorative tokens (glows, screws, grille, knurl...).
const decorative = /glow|screw|grille|knurl|knob|hatch|scanline|lip|backdrop|arc|dark-key|line/;
const materialColors = Object.entries(material.themes["instrument-light"])
  .filter(isColor)
  .map(([k]) => k)
  .filter((k) => !decorative.test(k));

export function Controls() {
  const [text, setText] = useState("");
  const [on, setOn] = useState(true);
  const [level, setLevel] = useState(40);
  const [pick, setPick] = useState("a");
  const [dial, setDial] = useState(62);
  const [small, setSmall] = useState(40);
  const [mode, setMode] = useState("log");
  return (
    <div className="page">
      <div className="page-header">
        <div className="page-heading">
          <span className="m-micro">Reference</span>
          <h1 className="page-title m-display">Controls</h1>
          <p className="page-subtitle">Every control and state on the active theme. Hover, active and focus are forced.</p>
        </div>
      </div>

      <Panel index="01" title="Button" meta="button.css + material">
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
        <Panel index="02" title="Text input">
          <TextField label="Default" value={text} onChange={setText} placeholder="Placeholder" />
          <TextField label="Focused" value="Bench 3" onChange={() => undefined} forceFocus />
          <TextField label="Invalid" value="10.0.4" onChange={() => undefined} error="Use host:port." mono />
          <TextField label="Disabled" value="read only" onChange={() => undefined} disabled />
          <Select label="Select" value="b" onChange={() => undefined} options={[{ value: "b", label: "48 kHz" }]} />
        </Panel>
        <Panel index="03" title="Selection">
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

      <Panel index="04" title="Instrument parts" meta="knob · segmented · meter · well">
        <div className="parts-row">
          <Knob label="Dial" value={dial} onChange={setDial} format={(v) => `${v}%`} />
          <div className="parts-stack">
            <span className="m-micro">Segmented</span>
            <Segmented label="Mode" value={mode} onChange={setMode}
              options={[{ value: "log", label: "Log" }, { value: "lin", label: "Lin" }, { value: "db", label: "dB" }]} />
            <span className="parts-lcd m-well m-chamfer">
              <span className="mono m-glow">48.20 MB/s</span>
              <span className="m-micro">LCD well</span>
            </span>
          </div>
          <Meter label="Level" left={0.55} right={0.9} />
          <div className="parts-band m-inverse">
            <Knob size="sm" label="Macro" value={small} onChange={setSmall} />
            <Segmented inverse label="Inverse mode" value={mode} onChange={setMode}
              options={[{ value: "log", label: "Log" }, { value: "lin", label: "Lin" }]} />
          </div>
        </div>
      </Panel>

      <div className="columns">
        <Panel index="05" title="Type">
          <span className="type-display m-display">Display 26/32 Plex Sans 600 (synthesized)</span>
          <span className="type-title">Title 18/24 IBM Plex Sans</span>
          <span>Body 14/20 IBM Plex Sans, the quick brown fox</span>
          <span className="m-micro">Micro label 11/16 mono uppercase</span>
          <span className="mono">Mono 0123456789 48.20 MB/s</span>
          <div className="row">
            {(["neutral", "accent", "success", "warning", "danger"] as const).map((t) => (
              <Badge key={t} tone={t}>
                {t}
              </Badge>
            ))}
          </div>
        </Panel>
        <Panel index="06" title="Colour tokens" meta={`${colorTokens.length} theme + ${materialColors.length} material`}>
          <div className="swatches">
            {[...colorTokens, ...materialColors].map((name) => (
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
