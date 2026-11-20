// SPDX-License-Identifier: MIT

// The hero panel: a light control deck of three numbered units (an LCD preset display with step keys,
// a power key with LED, big knobs, a coupling switch and a level meter) joined by scalloped notches to
// a dark macro band of small knobs on connector trees. Built from the same parts as the rest of the
// mock; its own drawing (notches, trees, grilles) is in console.css.
import { useState } from "react";
import { Knob } from "../components/Knob";
import { Meter } from "../components/Meter";
import { Segmented } from "../components/Segmented";
import "./console.css";

type Preset = { name: string; kind: string };

const presets: Record<string, Preset[]> = {
  input: [
    { name: "bus-a/pressure", kind: "SOURCE" },
    { name: "bus-a/temp", kind: "SOURCE" },
    { name: "bus-b/strain-04", kind: "SOURCE" },
  ],
  filter: [
    { name: "Low-pass 2nd", kind: "RESPONSE" },
    { name: "Band-pass", kind: "RESPONSE" },
    { name: "Notch 50 Hz", kind: "RESPONSE" },
  ],
  output: [
    { name: "Ring buffer", kind: "SINK" },
    { name: "CSV stream", kind: "SINK" },
  ],
};

function Lcd({ unit }: { unit: keyof typeof presets }) {
  const list = presets[unit];
  const [i, setI] = useState(0);
  const step = (d: number) => setI((i + d + list.length) % list.length);
  return (
    <div className="tconsole-lcdrow">
      <div className="tconsole-lcd m-well m-chamfer" aria-live="polite">
        <span className="tconsole-lcd-name mono m-glow">{list[i].name}</span>
        <span className="tconsole-lcd-meta m-micro">
          <span>{list[i].kind}</span>
          <span>
            {String(i + 1).padStart(2, "0")}/{String(list.length).padStart(2, "0")}
          </span>
        </span>
      </div>
      <button type="button" className="tconsole-step m-raised" aria-label="Previous preset" onClick={() => step(-1)}>
        <span className="tconsole-tri tconsole-tri-left" aria-hidden="true" />
      </button>
      <button type="button" className="tconsole-step m-raised" aria-label="Next preset" onClick={() => step(1)}>
        <span className="tconsole-tri" aria-hidden="true" />
      </button>
    </div>
  );
}

function Head({ index, title, on, setOn }: { index: string; title: string; on: boolean; setOn: (v: boolean) => void }) {
  return (
    <div className="tconsole-head">
      <span className="m-index">{index}</span>
      <h3 className="tconsole-title m-display">{title}</h3>
      <span className="m-engrave" aria-hidden="true" />
      <button
        type="button"
        className="tconsole-power"
        aria-label={`${title} power`}
        aria-pressed={on}
        onClick={() => setOn(!on)}
      >
        <svg className="tconsole-power-glyph" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M12 3v8" />
          <path d="M6.4 6.6a8 8 0 1 0 11.2 0" />
        </svg>
        <span className={`tconsole-power-led m-led ${on ? "m-led-warn" : ""}`} aria-hidden="true" />
      </button>
    </div>
  );
}

type Macro = { label: string; value: number };

function MacroGroup({ name, macros, set }: { name: string; macros: Macro[]; set: (i: number, v: number) => void }) {
  return (
    <div className={`tconsole-group tconsole-group-${macros.length}`} role="group" aria-label={name}>
      <span className="tconsole-group-name m-micro">{name}</span>
      <span className="tconsole-stem" aria-hidden="true" />
      <span className="tconsole-node" aria-hidden="true" />
      <div className="tconsole-macros">
        {macros.map((m, i) => {
          const pos = i === 0 ? "first" : i === macros.length - 1 ? "last" : "mid";
          return (
            <div key={m.label} className="tconsole-macro">
              <span className={`tconsole-branch tconsole-branch-${pos}`} aria-hidden="true" />
              <Knob size="sm" label={m.label} value={m.value} onChange={(v) => set(i, v)} />
            </div>
          );
        })}
      </div>
    </div>
  );
}

const notches = ["n1", "n2", "n3"];

export function Console() {
  const [power, setPower] = useState({ input: true, filter: true, output: false });
  const [gain, setGain] = useState(3);
  const [cutoff, setCutoff] = useState(4.2);
  const [q, setQ] = useState(0.7);
  const [mix, setMix] = useState(80);
  const [coupling, setCoupling] = useState("dc");
  const [groups, setGroups] = useState<Record<string, Macro[]>>({
    "A · Trim": [{ label: "Offset", value: 35 }, { label: "Scale", value: 55 }, { label: "Bias", value: 70 }],
    "B · Shape": [{ label: "Rise", value: 62 }, { label: "Fall", value: 20 }, { label: "Hold", value: 45 }, { label: "Decay", value: 75 }],
    "C · Output": [{ label: "Limit", value: 30 }, { label: "Dither", value: 55 }, { label: "Fade", value: 40 }],
  });
  const setMacro = (g: string) => (i: number, v: number) =>
    setGroups({ ...groups, [g]: groups[g].map((m, j) => (j === i ? { ...m, value: v } : m)) });

  return (
    <section className="tconsole" aria-label="Signal console">
      <div className="tconsole-deck">
        <span className="tconsole-grille tconsole-grille-l m-grille" aria-hidden="true" />
        <span className="tconsole-grille tconsole-grille-r m-grille" aria-hidden="true" />
        <div className="tconsole-unit">
          <Head index="01" title="Input" on={power.input} setOn={(v) => setPower({ ...power, input: v })} />
          <Lcd unit="input" />
          <div className="tconsole-controls">
            <div className="tconsole-switch">
              <span className="m-micro">Coupling</span>
              <Segmented label="Coupling" vertical value={coupling} onChange={setCoupling}
                options={[{ value: "dc", label: "DC" }, { value: "ac", label: "AC" }]} />
            </div>
            <Knob label="Gain" value={gain} onChange={setGain} min={-12} max={12} step={0.5}
              format={(v) => `${v > 0 ? "+" : ""}${v.toFixed(1)} dB`} />
          </div>
        </div>
        <div className="tconsole-unit tconsole-unit-wide tconsole-unit-seam">
          <Head index="02" title="Filter" on={power.filter} setOn={(v) => setPower({ ...power, filter: v })} />
          <Lcd unit="filter" />
          <div className="tconsole-controls">
            <Knob label="Cutoff" value={cutoff} onChange={setCutoff} min={0.1} max={20} step={0.1}
              format={(v) => `${v.toFixed(1)} kHz`} />
            <Knob label="Q" value={q} onChange={setQ} min={0.5} max={10} step={0.1} format={(v) => v.toFixed(1)} />
          </div>
        </div>
        <div className="tconsole-unit tconsole-unit-seam">
          <Head index="03" title="Output" on={power.output} setOn={(v) => setPower({ ...power, output: v })} />
          <Lcd unit="output" />
          <div className="tconsole-controls">
            <Knob label="Mix" value={mix} onChange={setMix} ends={["DRY", "WET"]} format={(v) => `${v}%`} />
            <Meter label="Output level" left={0.72} right={0.86} />
          </div>
        </div>
      </div>
      <div className="tconsole-band m-inverse">
        {notches.map((n) => (
          <svg key={n} className={`tconsole-notch tconsole-${n}`} viewBox="0 0 64 20" aria-hidden="true">
            <path d="M0 0H64C50 0 41 3 36 14Q32 21 28 14C23 3 14 0 0 0Z" />
          </svg>
        ))}
        {Object.entries(groups).map(([name, macros]) => (
          <MacroGroup key={name} name={name} macros={macros} set={setMacro(name)} />
        ))}
      </div>
    </section>
  );
}
