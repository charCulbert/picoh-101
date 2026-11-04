// The link to the synth, and how its values read (as synth/paramText.h).
import { parameters, panelUnits, type Parameter } from "./params.generated.js";

export const unitOf = (p: Parameter): string => panelUnits[p.unit] ?? "";

const names = new Map(parameters.filter((p) => p.choices).map((p) => [p.name, p.choices!]));

export const isChoice = (name: string): boolean => names.has(name);

// A parameter's value as text; min is its minimum, which makes a % bipolar when below 0.
export function formatValue(name: string, unit: string, value: number, min = 0): string {
    const list = names.get(name);
    if (list) return list[Math.min(Math.max(Math.trunc(value) || 0, 0), list.length - 1)];
    if (unit === "Hz")
        return value < 10 ? value.toFixed(2) + " Hz" : value < 1000 ? Math.round(value) + " Hz"
                                                     : (value / 1000).toFixed(2) + " kHz";
    if (unit === "s") return value < 1 ? Math.round(value * 1000) + " ms" : value.toFixed(2) + " s";
    if (unit === "%") {
        const shown = Math.round(value * 10000) / 100; // up to two decimals
        return (min < 0 && shown > 0 ? "+" : "") + (shown || 0) + "%";
    }
    if (unit === "cents") return (value < 0 ? "" : "+") + Math.round(value) + " ct";
    return value.toFixed(2);
}

// The text a value field opens on: a choice's name, or the bare number in the unit typed numbers count in.
export function editText(name: string, unit: string, value: number): string {
    if (names.has(name)) return formatValue(name, unit, value);
    const shown = unit === "s" ? value * 1000 : unit === "%" ? value * 100 : value;
    return String(Number(shown.toFixed(unit === "%" ? 2 : 3)));
}

export interface ValueInfo { name: string; unit: string; min: number; max: number }

// A value typed for a parameter, as formatValue writes it or close: "300 ms", "1.2 s", "440 Hz",
// "1.2 kHz", "85%", "-12 ct", a bare number in the unit shown (ms for times), or a choice's name, case
// and spaces ignored. Clamped to the range; null for anything else.
export function parseValue(text: string, { name, unit, min, max }: ValueInfo): number | null {
    const plain = String(text).toLowerCase().replace(/\s+/g, "");
    const list = names.get(name);
    if (list) {
        const index = list.findIndex((choice) => choice.toLowerCase().replace(/\s+/g, "") === plain);
        return index < 0 ? null : Math.min(Math.max(index, min), max);
    }
    const match = /^([+-]?(?:\d+\.?\d*|\.\d+))([a-z%]*)$/.exec(plain);
    if (!match) return null;
    const units: Record<string, Record<string, number>> = {
        s: { "": 0.001, ms: 0.001, s: 1 },
        Hz: { "": 1, hz: 1, khz: 1000 },
        "%": { "": 0.01, "%": 0.01 },
        cents: { "": 1, ct: 1, cent: 1, cents: 1 },
    };
    const scale = (units[unit] ?? { "": 1 })[match[2]];
    if (scale === undefined) return null;
    return Math.min(Math.max(Number(match[1]) * scale, min), max);
}

// Anything that carries the GUI lines as a WebSocket does: the Pico's WebSocket, USB serial or SysEx
// (usb.ts), the app's IPC, or the plugin's bridge (bridge.ts).
export interface LineSocket {
    readyState: number; // 0 connecting, 1 open, 3 closed, as WebSocket
    transport?: string; // for people: "USB serial", "USB MIDI", or ""
    steady?: boolean;   // never drops silently, so no once-a-second load is needed to know it's there
    onopen: (() => void) | null;
    onclose: (() => void) | null;
    onerror: (() => void) | null;
    onmessage: ((event: { data: unknown }) => void) | null;
    send(line: string): void;
    close(): void;
}

export type OpenSocket = (url: string) => LineSocket;

// The keys line's 32 hex digits: four 8-digit words, key 32w+n is bit n of word w.
function keysFrom(hex: string): Set<number> {
    const down = new Set<number>();
    for (let w = 0; w < 4; w++) {
        const word = parseInt(hex.slice(w * 8, w * 8 + 8), 16);
        for (let n = 0; n < 32; n++) if ((word >>> n) & 1) down.add(w * 32 + n);
    }
    return down;
}

export interface LiveHandlers {
    value(id: number, value: number): void;
    wheel(kind: "b" | "m", value: number): void;
    sequencer?(mode: string, position: number, length: number): void;
    keyTranspose?(semitones: number): void;
    patch?(armed: string, key: number): void;
    key?(note: number, down: boolean): void;
    keys?(down: Set<number>): void; // every key down, so stale ones go out
    load(average: number, busiest: number, late: number): void;
}

// LiveView: the link to the Pico. It reconnects through any drop, and the synth resends every value when
// it's back; 2.5 s without its once-a-second load means it's gone.
export class LiveView {
    socket: LineSocket | null = null;
    open = false;
    failures = 0;  // attempts in a row that never opened
    lastReply = 0; // performance.now() of the last message; 0 before the first
    openedAt = 0;
    private openSocket: OpenSocket;
    private readonly firstOpenSocket: OpenSocket;
    private retry = 0;
    private readonly url: string;

    constructor(private readonly handlers: LiveHandlers, { host, openSocket }: { host?: string | null; openSocket?: OpenSocket } = {}) {
        this.openSocket = openSocket ?? ((url) => new WebSocket(url) as unknown as LineSocket);
        this.firstOpenSocket = this.openSocket;
        this.url = `ws://${host || location.host}/ws`;
        // Try at once instead of waiting out the back-off when the tab, window or network comes back.
        const kick = () => { if (document.visibilityState === "visible") this.reconnectNow(); };
        addEventListener("online", kick);
        addEventListener("focus", kick);
        document.addEventListener("visibilitychange", kick);
        // Open but silent for 5 s: the connection is dead without knowing it. Start over.
        setInterval(() => { if (this.open && !this.socket?.steady && performance.now() - this.lastReply > 5000) this.socket?.close(); }, 1000);
        this.connect();
    }

    // Another transport (usb.ts), or null for the first one.
    use(openSocket: OpenSocket | null): void {
        this.openSocket = openSocket ?? this.firstOpenSocket;
        this.failures = 0;
        const old = this.socket;
        this.socket = null; // so the old one's close doesn't retry
        this.open = false;
        old?.close();
        clearTimeout(this.retry);
        this.connect();
    }

    get transport(): string { return this.socket?.transport ?? ""; }
    get down(): boolean { return !this.open || (!this.socket?.steady && performance.now() - this.lastReply > 2500); }
    get everConnected(): boolean { return this.lastReply > 0; }

    private connect(): void {
        clearTimeout(this.retry);
        this.retry = 0;
        const ws = this.openSocket(this.url);
        this.socket = ws;
        // An attempt made while the synth was away can hang without an answer; give up on it.
        const giveUp = setTimeout(() => { if (ws.readyState === 0) ws.close(); }, 3000);
        ws.onopen = () => {
            clearTimeout(giveUp);
            this.open = true;
            this.openedAt = performance.now();
            this.failures = 0;
            this.lastReply = performance.now();
        };
        ws.onclose = () => {
            clearTimeout(giveUp);
            if (this.socket !== ws) return;
            if (!this.open) this.failures++;
            this.open = false;
            this.socket = null;
            this.retry = setTimeout(() => this.connect(), this.failures < 3 ? 1000 : 3000);
        };
        ws.onerror = () => ws.close();
        ws.onmessage = (e) => {
            this.lastReply = performance.now();
            const [kind, a, b, c] = String(e.data).split(" ");
            if (kind === "s") return this.handlers.sequencer?.(a, Number(b), Number(c));
            if (kind === "t") return this.handlers.keyTranspose?.(Number(a));
            if (kind === "p") return this.handlers.patch?.(a, Number(b));
            if (kind === "v") this.handlers.value(Number(a), Number(b));
            else if (kind === "b" || kind === "m") this.handlers.wheel(kind, Number(a));
            else if (kind === "k") this.handlers.key?.(Number(a), b === "1");
            else if (kind === "keys" && a?.length === 32) this.handlers.keys?.(keysFrom(a));
            else if (kind === "load") this.handlers.load(Number(a), Number(b), Number(c));
        };
    }

    reconnectNow(): void {
        if (this.socket?.readyState === 1 && !this.down) return;
        if (this.socket?.readyState === 1) return this.socket.close(); // open but silent: onclose retries
        this.failures = Math.min(this.failures, 2);
        if (!this.socket) this.connect();
    }

    send(line: string): void { if (this.socket?.readyState === 1) this.socket.send(line); }
    set(id: number, value: number): void { this.send(`v ${id} ${Number(value.toPrecision(7))}`); }
    gesture(id: number, on: boolean): void { this.send(`g ${id} ${on ? 1 : 0}`); } // a drag begins or ends
    noteOn(note: number, velocity: number): void { this.send(`n ${note} ${velocity}`); }
    noteOff(note: number): void { this.send(`n ${note} 0`); }
    allNotesOff(): void { this.send("a"); }
    bend(value: number): void { this.send(`b ${value.toFixed(4)}`); } // pitch wheel, -1 to 1
    mod(value: number): void { this.send(`m ${value.toFixed(4)}`); }  // mod wheel, 0 to 1
    sequencer(button: "load" | "play" | "rest"): void { this.send(`s ${button}`); }
    legato(down: boolean): void { this.send(`l ${down ? 1 : 0}`); }
    keyTranspose(down: boolean): void { this.send(`t ${down ? 1 : 0}`); }
    patch(button: "load" | "store"): void { this.send(`p ${button}`); }
}
