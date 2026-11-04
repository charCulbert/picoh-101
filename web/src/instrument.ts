// The SH-101 panel, drawn in Compost, measured from photos. Labels are only those printed on the panel.
//
import { formatValue, editText, parseValue, unitOf } from "./live.js";
import type { Parameter } from "./params.generated.js";
import type { ParameterEventDetail } from "../vendor/compost/src/utils.js";
import type { CompostButton } from "../vendor/compost/src/components/compost-button.js";
import type { CompostKnob } from "../vendor/compost/src/components/compost-knob.js";
import type { CompostSlider } from "../vendor/compost/src/components/compost-slider.js";
import type { CompostPiano } from "../vendor/compost/src/components/compost-piano.js";
import "../vendor/compost/src/components/compost-button.js";
import "../vendor/compost/src/components/compost-knob.js";
import "../vendor/compost/src/components/compost-piano.js";
import "../vendor/compost/src/components/compost-slider.js";

// What the panel calls as it's played.
export interface PanelLink {
    set(id: number, value: number): void;
    begin?(id: number): void;
    end?(id: number): void;
    bend(value: number): void;
    mod(value: number): void;
    sequencer(button: "load" | "play" | "rest"): void;
    legato(down: boolean): void;
    keyTranspose(down: boolean): void;
    patch(button: "load" | "store"): void;
    noteOn(note: number): void;
    noteOff(note: number): void;
    releaseNotes(): void;
    say(text: string): void;
}

// What the panel shows from elsewhere.
export interface Panel {
    show(id: number, value: number, fromElsewhere: boolean): void;
    wheel(kind: "b" | "m", value: number): void;
    sequencer(mode: string, position: number, length: number): void;
    keyTranspose(semitones: number): void;
    patch(armed: string, key: number): void; // the button armed ("off", "load", "store"), the patch's key (0: none)
    key(note: number, down: boolean): void; // a key sounding or not, from anywhere: lit on the keyboard
    keys(down: Set<number>): void;          // exactly these lit (keys held here stay lit)
    clearKeys(): void;
}

// The slider takes the knob's text hooks too, which compost-slider.d.ts doesn't declare.
type Fader = CompostKnob | (CompostSlider & Pick<CompostKnob, "formatValue" | "formatEditText" | "parseEditText">);
interface Shown { p: Parameter; box: Element; set(value: number): void; isHeld?(): boolean; editing?(): boolean }
const detail = (e: Event) => (e as CustomEvent<ParameterEventDetail>).detail;

// The bender lever: sideways bends the pitch, -1 to 1; pushing it up raises the mod wheel, 0 to 1.
// Let go and both spring back to the centre. It shows what comes in from elsewhere while it isn't held.
interface Lever {
    el: HTMLElement;
    show(kind: "b" | "m", value: number): void;
}
function benderLever(onMove: (bend: number, mod: number) => void): Lever {
    const el = document.createElement("div");
    el.className = "lever";
    el.innerHTML = `<div class="lever-track" role="slider" aria-label="Bender"><span class="lever-slot"></span><svg class="lever-stem" aria-hidden="true"><line x1="50%" y1="115%" /></svg><span class="lever-thumb"></span></div>
<span class="lever-arrow" aria-hidden="true"></span><span class="under-label">BENDER</span>`;
    const track = el.querySelector<HTMLElement>(".lever-track")!;
    const thumb = el.querySelector<HTMLElement>(".lever-thumb")!;
    const stem = el.querySelector("line")!;
    let bend = 0, mod = 0, held = false;
    // A drag moves it from where it is, along whichever axis moved first; Shift frees both.
    let startX = 0, startY = 0, startBend = 0, startMod = 0, axis: "" | "x" | "y" = "";
    const draw = () => {
        // The handle's centre, in % of the track: on the slot at rest, toward the top when pushed.
        const x = `${50 + bend * 40}%`, y = `${72 - mod * 44}%`;
        thumb.style.left = x;
        thumb.style.top = y;
        stem.setAttribute("x2", x);
        stem.setAttribute("y2", y);
    };
    const move = (e: PointerEvent) => {
        const r = track.getBoundingClientRect();
        const dx = e.clientX - startX, dy = startY - e.clientY;
        if (!axis && Math.hypot(dx, dy) >= 4) axis = Math.abs(dx) >= Math.abs(dy) ? "x" : "y";
        if (!axis) return;
        const clamp = (v: number, lo: number) => Math.min(1, Math.max(lo, v));
        if (e.shiftKey || axis === "x") bend = clamp(startBend + (dx / r.width) * 2, -1);
        if (e.shiftKey || axis === "y") mod = clamp(startMod + dy / r.height, 0);
        draw();
        onMove(bend, mod);
    };
    const letGo = () => {
        if (!held) return;
        held = false;
        bend = mod = 0;
        draw();
        onMove(0, 0);
    };
    track.addEventListener("pointerdown", (e) => {
        held = true;
        track.setPointerCapture(e.pointerId);
        startX = e.clientX;
        startY = e.clientY;
        startBend = bend;
        startMod = mod;
        axis = "";
    });
    track.addEventListener("pointermove", (e) => { if (held) move(e); });
    track.addEventListener("pointerup", letGo);
    track.addEventListener("pointercancel", letGo);
    draw();
    return {
        el,
        show(kind, value) {
            if (held) return;
            if (kind === "b") bend = value;
            else mod = value;
            draw();
        },
    };
}

const TOP = ["TUNE", "MODULATOR", "VCO", "SOURCE MIXER", "VCF", "VCA", "ENV"];

// Printed as waveforms rather than words.
const PULSE = `<svg viewBox="0 0 16 8" aria-hidden="true"><path d="M1 7V1h5v6h5V1h4" /></svg>`;
const SAW = `<svg viewBox="0 0 16 8" aria-hidden="true"><path d="M1 7L7 1v6l6-6v6" /></svg>`;
const GLYPHS: Record<string, string> = { pulse: PULSE, saw: SAW };
const wave = (d: string) => `<svg viewBox="0 0 16 8" aria-hidden="true"><path d="${d}" /></svg>`;
// Printed over a selector knob, one per position, as on the panel: a symbol or a word.
const DIAL_SWEEP = 100;
const DIAL_MARKS: Record<string, string[]> = {
    "lfo wave": [wave("M1 7L4.5 1L8 7L11.5 1L15 7"), wave("M1 7V1h7v6h7"), "RANDOM", "NOISE"],
};

const markup = `<div class="sh">
<div class="band"></div>
<div class="top"></div>
<div class="buttons">
<div class="grp grp-seq"><div class="row seq"></div><span class="bracket">SEQUENCER</span></div>
<div class="grp grp-arp"><div class="row arp"></div><span class="bracket">ARPEGGIO</span></div>
<div class="grp grp-hold"><div class="row hold"></div></div>
<div class="grp grp-patch"><div class="row patch"></div><span class="bracket">PATCH</span></div>
<span class="reach"></span>
<span class="logo">PICOH-101</span>
</div>
<div class="controllers"></div>
<div class="keys-side"><compost-piano class="keys" inline root-note="41" note-count="32" aria-label="Keyboard"></compost-piano></div>
</div>`;

export function createInstrument(root: HTMLElement, params: Parameter[], live: PanelLink): Panel {
    root.innerHTML = markup;
    const q = <T extends Element = HTMLElement>(s: string) => root.querySelector<T>(s)!;
    const byKey = new Map(params.map((p) => [p.key, p]));
    const controls = new Map<number, Shown>();

    const say = (p: Parameter, value: number) => live.say(`${p.name}  ${formatValue(p.name, unitOf(p), value, p.min)}`);

    function boxFor(el: Element, label: string, className: string, title = "") {
        const box = document.createElement("div");
        box.className = `ctl ${className}`;
        if (title) box.title = title;
        const name = document.createElement("small");
        if (GLYPHS[label]) name.innerHTML = GLYPHS[label];
        else name.textContent = label ?? "";
        box.append(name, el);
        return box;
    }

    function element(p: Parameter, orientation: string) {
        const info = { name: p.name, unit: unitOf(p), min: p.min, max: p.max };
        const knob = p.control === "knob" || p.control === "selector";
        const el = (knob ? document.createElement("compost-knob") : document.createElement("compost-slider")) as Fader;
        if (!knob) el.setAttribute("orientation", orientation);
        const attrs: Record<string, string | number> = { min: p.min, max: p.max, "reset-value": p.default, label: p.name, editable: "" };
        if (p.stepped) attrs.step = 1;
        if (p.log) Object.assign(attrs, { curve: "log", mid: p.mid });
        else if (!p.stepped) attrs.mid = p.mid;
        if (p.choices) attrs.text = p.choices.join("|");
        for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
        el.formatValue = (v: number) => formatValue(p.name, info.unit, v, p.min);
        el.formatEditText = (v: number) => editText(p.name, info.unit, v);
        el.parseEditText = (t: string) => parseValue(t, info);
        let held = false;
        el.addEventListener("parameter-begin", () => { held = true; live.begin?.(p.id); });
        el.addEventListener("parameter-edit", (e) => { live.set(p.id, detail(e).value); say(p, detail(e).value); });
        el.addEventListener("parameter-end", (e) => { held = false; live.set(p.id, detail(e).value); live.end?.(p.id); });
        el.value = p.default;
        return { el, isHeld: () => held, editing: () => Boolean((el as Fader & { isEditingValue?: boolean }).isEditingValue) };
    }

    // A fader with its scale, a slide switch with its positions printed beside it, or a knob.
    function control(p: Parameter, { horizontal = false, label = p.short }: { horizontal?: boolean; label?: string } = {}) {
        const title = p.name;
        const c = element(p, horizontal ? "horizontal" : "vertical");
        let box: HTMLElement;
        if (p.control === "switch") {
            // A switch: a square button that steps to the next position on each click (shift-click
            // goes back), its positions printed beside it with the current one lit. A printed
            // position can be clicked to go straight to it.
            const el = document.createElement("compost-button");
            el.setAttribute("mode", "cycle");
            const choices = p.choices ?? [];
            el.setAttribute("text", choices.join("|"));
            el.setAttribute("aria-label", p.name);
            el.setAttribute("value", String(p.default));
            const holder = document.createElement("span");
            holder.className = horizontal ? "switch switch-h" : "switch";
            const marks = document.createElement("span");
            marks.className = "marks";
            const order = choices.map((name, i): [string, number] => [name, i]);
            if (!horizontal) order.reverse(); // the top position is the last choice, as printed
            const buttons = new Map<number, HTMLButtonElement>();
            for (const [name, i] of order) {
                const b = document.createElement("button");
                b.type = "button";
                b.tabIndex = -1;
                b.textContent = name;
                b.addEventListener("click", () => el.setValue(i, true, "control"));
                marks.append(b);
                buttons.set(i, b);
            }
            const light = () => { for (const [i, b] of buttons) b.classList.toggle("on", i === el.value); };
            el.addEventListener("parameter-edit", (e) => { live.set(p.id, detail(e).value); say(p, detail(e).value); light(); });
            light();
            holder.append(el, marks);
            box = boxFor(holder, label, horizontal ? "ctl-switch-h" : "ctl-switch", title);
            controls.set(p.id, { p, box, set: (v) => { el.value = v; light(); } });
            return box;
        } else if (p.control === "fader") {
            const holder = document.createElement("span");
            holder.className = "fader";
            const scale = document.createElement("span");
            scale.className = "scale";
            holder.append(scale, c.el);
            box = boxFor(holder, label, "ctl-fader", title);
        } else if (DIAL_MARKS[p.key]) {
            // A tick and a mark at each position's angle; click a mark to go to it.
            const holder = document.createElement("span");
            holder.className = "dial";
            c.el.setAttribute("sweep", String(DIAL_SWEEP));
            const marks = DIAL_MARKS[p.key];
            marks.forEach((mark, i) => {
                const angle = (i / (marks.length - 1) - 0.5) * DIAL_SWEEP;
                const tick = document.createElement("i");
                tick.style.setProperty("--angle", `${angle}deg`);
                const b = document.createElement("button");
                b.type = "button";
                b.tabIndex = -1;
                b.title = p.choices?.[i] ?? "";
                b.className = angle > 0 ? "right" : "";
                b.innerHTML = mark.startsWith("<svg") ? mark : `<span>${mark}</span>`;
                const r = angle * Math.PI / 180;
                b.style.left = `calc(50% + ${(Math.sin(r) * 2.75).toFixed(3)}em)`;
                b.style.top = `calc(50% - ${(Math.cos(r) * 2.75).toFixed(3)}em)`;
                b.addEventListener("click", () => {
                    c.el.value = i;
                    live.set(p.id, i);
                    say(p, i);
                });
                holder.append(tick, b);
            });
            holder.append(c.el);
            box = boxFor(holder, label, `ctl-${p.control}`, title);
        } else {
            box = boxFor(c.el, label, `ctl-${p.control}`, title);
        }
        controls.set(p.id, { p, box, set: (v) => { c.el.value = v; }, isHeld: c.isHeld, editing: c.editing });
        return box;
    }

    // A push button with its LED above and its names under (the second is its sequencer meaning).
    function pushButton(label: string, second: string, onChange: (pressed: boolean) => void): { el: CompostButton; box: HTMLElement } {
        const el = document.createElement("compost-button");
        el.setAttribute("mode", "switch");
        el.setAttribute("aria-label", second ? `${label} / ${second}` : label);
        el.addEventListener("change", () => onChange(el.pressed));
        const box = document.createElement("div");
        box.className = "ctl ctl-button";
        box.innerHTML = `<i class="led"></i>`;
        box.append(el);
        const names = document.createElement("small");
        names.innerHTML = `${label}${second ? `<br><span class="second">${second}</span>` : ""}`;
        box.append(names);
        return { el, box };
    }

    // A button that acts while held, its LED set from the synth: LOAD, PLAY, HOLD, KEY TRANSPOSE.
    function holdButton(label: string, second: string, onPress: (down: boolean) => void): { box: HTMLElement; light(on: boolean): void } {
        const el = document.createElement("compost-button");
        el.setAttribute("mode", "trigger");
        el.setAttribute("aria-label", second ? `${label} / ${second}` : label);
        const box = document.createElement("div");
        box.className = "ctl ctl-button";
        box.innerHTML = `<i class="led"></i>`;
        box.append(el);
        const names = document.createElement("small");
        names.innerHTML = `${label}${second ? `<br><span class="second">${second}</span>` : ""}`;
        box.append(names);
        let down = false;
        const set = (now: boolean) => {
            if (now === down) return;
            down = now;
            box.classList.toggle("down", now);
            onPress(now);
        };
        el.addEventListener("pointerdown", (e) => { el.setPointerCapture(e.pointerId); set(true); });
        for (const type of ["pointerup", "pointercancel", "lostpointercapture"]) el.addEventListener(type, () => set(false));
        el.addEventListener("keydown", (e) => { if ((e.key === " " || e.key === "Enter") && !e.repeat) set(true); });
        el.addEventListener("keyup", (e) => { if (e.key === " " || e.key === "Enter") set(false); });
        return { box, light: (on) => box.classList.toggle("lit", on) };
    }

    // The top sections. TUNE has no section name printed, only the knob's.
    const bySection = new Map<string, Parameter[]>();
    for (const p of [...params].sort((a, b) => a.slot - b.slot)) {
        if (!bySection.has(p.section)) bySection.set(p.section, []);
        bySection.get(p.section)!.push(p);
    }
    for (const name of TOP) {
        if (!bySection.has(name)) continue;
        const box = document.createElement("section");
        box.className = "sect";
        box.dataset.section = name;
        box.innerHTML = `<h3>${name === "TUNE" ? "" : name}</h3><div class="row"></div>`;
        for (const p of bySection.get(name)!) box.querySelector(".row")!.append(control(p, { label: GLYPHS[p.key] ? p.key : p.short }));
        q(".top").append(box);
    }

    // The lower left, as printed: the two knobs, the bender's sliders, the switches, and the lever.
    const left = q(".controllers");
    left.innerHTML = `
<div class="c-volume"></div>
<div class="c-portamento"></div>
<div class="c-bend"><div class="c-bend-faders"></div><span class="bracket-u" aria-hidden="true"></span><span class="under-label">BENDER</span></div>
<div class="c-lfo"></div>
<div class="c-switches"><span class="link-down" aria-hidden="true"></span></div>
<div class="c-arrow" aria-hidden="true"></div>
<div class="c-lever"></div>`;
    const put = (selector: string, key: string, options?: { horizontal?: boolean; label?: string }) => {
        const p = byKey.get(key);
        if (p) left.querySelector(selector)!.append(control(p, options));
    };
    put(".c-volume", "volume");
    put(".c-portamento", "portamento");
    put(".c-bend-faders", "bend vco");
    put(".c-bend-faders", "bend vcf");
    put(".c-lfo", "lfo mod", { label: "LFO MOD" });
    put(".c-switches", "portamento mode", { horizontal: true, label: "PORTAMENTO" });
    put(".c-switches", "transpose", { horizontal: true, label: "TRANSPOSE" });

    let sentBend = 0, sentMod = 0;
    const lever = benderLever((b, m) => {
        if (b !== sentBend) live.bend(sentBend = b);
        if (m !== sentMod) live.mod(sentMod = m);
    });
    left.querySelector(".c-lever")!.append(lever.el);

    // The sequencer. While loading, HOLD is LEGATO and KEY TRANSPOSE is REST.
    let seqMode = "stopped";
    const load = holdButton("LOAD", "", (down) => { if (down) live.sequencer("load"); });
    const play = holdButton("PLAY", "", (down) => { if (down) live.sequencer("play"); });
    q(".seq").append(load.box, play.box);

    const arpParam = byKey.get("arpeggio")!;
    const arp: { el: CompostButton; value: number }[] = ([["DOWN", 1], ["U&D", 2], ["UP", 3]] as const).map(([label, value]) => {
        const b = pushButton(label, "", (pressed) => {
            const v = pressed ? value : 0;
            for (const other of arp) if (other.el !== b.el) other.el.pressed = false;
            live.set(arpParam.id, v);
            say(arpParam, v);
        });
        q(".arp").append(b.box);
        return { el: b.el, value };
    });
    controls.set(arpParam.id, { p: arpParam, box: q(".arp"), set: (v) => { for (const a of arp) a.el.pressed = a.value === v; } });

    const holdParam = byKey.get("hold")!;
    let holdOn = false, legatoDown = false, restDown = false;
    const hold = holdButton("HOLD", "LEGATO", (down) => {
        if (down && seqMode === "loading") legatoDown = true;
        if (legatoDown) {
            live.legato(down);
            if (!down) legatoDown = false;
            return;
        }
        if (!down) return;
        holdOn = !holdOn;
        hold.light(holdOn);
        live.set(holdParam.id, holdOn ? 1 : 0);
        say(holdParam, holdOn ? 1 : 0);
    });
    q(".hold").append(hold.box);
    controls.set(holdParam.id, { p: holdParam, box: hold.box, set: (v) => { holdOn = v >= 1; hold.light(holdOn); } });
    const keyTranspose = holdButton("KEY TRANSPOSE", "REST", (down) => {
        if (down && seqMode === "loading") restDown = true;
        if (restDown) {
            if (down) live.sequencer("rest");
            else restDown = false;
            return;
        }
        live.keyTranspose(down);
        if (down) live.say("KEY TRANSPOSE: press a key (the lowest C for none)");
    });
    q(".hold").append(keyTranspose.box);

    // Patch memory, one per key: arm LOAD or STORE (again: cancel), then press a key.
    let patchArmed = "off", patchKey = 0;
    const patchButtons = (["load", "store"] as const).map((button) => {
        const b = holdButton(button.toUpperCase(), "", (down) => { if (down) live.patch(button); });
        q(".patch").append(b.box);
        return b;
    });

    // The keys: 32, F to C. Every way a key can be let go sends its note off; if the page goes
    // before it can, the synth lets go itself.
    const piano = q<CompostPiano>(".keys");
    const note = (e: Event) => (e as CustomEvent<{ note: number }>).detail.note;
    piano.addEventListener("note-down", (e) => live.noteOn(note(e)));
    piano.addEventListener("note-up", (e) => live.noteOff(note(e)));
    const letGo = () => { piano.allNotesOff(); live.releaseNotes(); };
    document.addEventListener("visibilitychange", () => { if (document.hidden) letGo(); });
    addEventListener("pagehide", letGo);
    addEventListener("blur", letGo);
    // Upright phone: fewer, wider keys (two octaves from F); otherwise the panel's 32.
    const upright = matchMedia("(max-width: 700px) and (min-height: 521px)");
    const markPatch = () => {
        for (const el of piano.shadowRoot?.querySelectorAll<HTMLElement>("[data-note]") ?? [])
            el.part.toggle("patch", Number(el.dataset.note) === patchKey);
    };
    const fitKeys = () => { piano.setAttribute("note-count", upright.matches ? "25" : "32"); markPatch(); };
    upright.addEventListener("change", fitKeys);
    fitKeys();
    const keysSide = q(".keys-side");
    new ResizeObserver(() => {
        if (piano.keyboardWidth) piano.style.fontSize = 16 * keysSide.clientWidth / piano.keyboardWidth + "px";
    }).observe(keysSide);

    return {
        show(id, value, fromElsewhere) {
            const c = controls.get(id);
            if (!c || c.isHeld?.() || c.editing?.()) return;
            c.set(value);
            if (!fromElsewhere) return;
            say(c.p, value);
            c.box.classList.add("moved");
            setTimeout(() => c.box.classList.remove("moved"), 60);
        },
        wheel(kind, value) { lever.show(kind, value); },
        sequencer(mode, position, length) {
            const was = seqMode;
            seqMode = mode;
            load.light(mode === "loading");
            play.light(mode === "playing");
            if (mode === "loading") live.say(`LOAD  ${position} step${position === 1 ? "" : "s"}`);
            else if (mode === "playing") live.say(`PLAY  step ${position} of ${length}`);
            else if (was !== "stopped") live.say(length ? `Sequence: ${length} step${length === 1 ? "" : "s"}` : "No sequence");
        },
        keyTranspose(semitones) {
            keyTranspose.light(semitones !== 0);
            if (semitones) live.say(`KEY TRANSPOSE  ${semitones > 0 ? "+" : ""}${semitones}`);
        },
        patch(armed, key) {
            patchButtons[0].light(armed === "load");
            patchButtons[1].light(armed === "store");
            if (armed !== patchArmed && armed !== "off") live.say(`${armed.toUpperCase()}: press a key for the patch`);
            else if (key !== patchKey && key) live.say(`Patch ${key - 40}`);
            patchArmed = armed;
            patchKey = key;
            markPatch();
        },
        key(note, down) { piano.handleMIDIMessage([down ? 0x90 : 0x80, note, down ? 100 : 0]); },
        keys(down) {
            for (let note = 0; note < 128; note++) {
                const want = down.has(note);
                if (want !== piano.isNoteActive(note)) piano.handleMIDIMessage([want ? 0x90 : 0x80, note, want ? 100 : 0]);
            }
        },
        clearKeys() { piano.allNotesOff(); },
    };
}
