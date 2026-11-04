// The page: the panel (instrument.ts) and its link to the synth (live.ts). The synth keeps every value.
// The same page is the plugin's GUI: its index.html sets window.__picohPlugin, and there it shows the
// panel alone, scaled to the window, linked through the webview bridge (bridge.ts).
import { parameters } from "./params.generated.js";
import { LiveView, type OpenSocket } from "./live.js";
import { createInstrument } from "./instrument.js";
import { USB_SERIAL, hasSerial, hasMidi, serialSocket, midiSocket } from "./usb.js";

// The synth: the Pico that served the page, or a Pico named in the address (?pico=host).
const query = new URLSearchParams(location.search);
const picoHost = query.get("pico") || query.get("host");
// In the app (app/), the Pico over the USB cable, through the app.
const inApp = !!window.__picohApp;
const inPlugin = !!window.__picohPlugin;
const openSocket: OpenSocket | undefined = inPlugin ? (await import("./bridge.js")).bridgeSocket()
    : inApp ? () => window.__picohApp!.openSocket() : undefined;

// The page's own elements; the plugin's page has none of them, and the code that uses them doesn't run there.
const $ = (id: string) => document.getElementById(id)!;
const playing = new Set<number>();

// Theme: system, dark or light, remembered on this device.
function setTheme(choice: string) {
    if (choice === "system") document.documentElement.removeAttribute("data-theme");
    else document.documentElement.dataset.theme = choice;
    for (const b of document.querySelectorAll<HTMLElement>("[data-theme-choice]"))
        b.setAttribute("aria-pressed", String(b.dataset.themeChoice === choice));
    try { localStorage.setItem("theme", choice); } catch {}
}
for (const b of document.querySelectorAll<HTMLElement>("[data-theme-choice]"))
    b.addEventListener("click", () => setTheme(b.dataset.themeChoice ?? "system"));
let saved = "system";
try { saved = localStorage.getItem("theme") || "system"; } catch {}
setTheme(saved);

const link = new LiveView({
    // Not the values resent on connecting: those only set the controls.
    value: (id, value) => panel.show(id, value, performance.now() - link.openedAt > 500),
    wheel: (kind, value) => panel.wheel(kind, value),
    sequencer: (mode, position, length) => panel.sequencer(mode, position, length),
    keyTranspose: (semitones) => panel.keyTranspose(semitones),
    patch: (armed, key) => panel.patch(armed, key),
    key: (note, down) => panel.key(note, down),
    keys: (down) => panel.keys(down),
    load: showLoad,
}, { host: picoHost, openSocket });

const panel = createInstrument($("instrument"), parameters, {
    say: (text) => { const readout = document.getElementById("readout"); if (readout) readout.textContent = text; },
    set: (id, value) => link.set(id, value),
    begin: (id) => link.gesture(id, true),
    end: (id) => link.gesture(id, false),
    bend: (v) => link.bend(v),
    mod: (v) => link.mod(v),
    sequencer: (button) => link.sequencer(button),
    legato: (down) => link.legato(down),
    keyTranspose: (down) => link.keyTranspose(down),
    patch: (button) => link.patch(button),
    noteOn(note) { playing.add(note); link.noteOn(note, 0.8); },
    noteOff(note) { if (playing.delete(note)) link.noteOff(note); },
    releaseNotes() { for (const note of [...playing]) this.noteOff(note); },
});

// USB: Web Serial or Web MIDI, remembered so the next visit reconnects. Both need https or localhost.
const TRANSPORT_KEY = "transport";
async function useUsb(kind: string, ask: boolean) {
    try {
        if (kind === "serial") {
            const ports = await navigator.serial.getPorts();
            const port = ports.find((p) => USB_SERIAL.some((f) => p.getInfo().usbVendorId === f.usbVendorId && p.getInfo().usbProductId === f.usbProductId))
                ?? (ask ? await navigator.serial.requestPort({ filters: USB_SERIAL }) : null);
            if (!port) return;
            link.use(serialSocket(port));
        } else {
            link.use(midiSocket(await navigator.requestMIDIAccess({ sysex: true })));
        }
        localStorage.setItem(TRANSPORT_KEY, kind);
    } catch (error) {
        if (ask) $("readout").textContent = `USB: ${(error as Error).message}`;
    }
    showUsb();
}
function stopUsb() {
    localStorage.removeItem(TRANSPORT_KEY);
    link.use(null);
    showUsb();
}
function showUsb() {
    const kind = localStorage.getItem(TRANSPORT_KEY);
    $("usb-serial").setAttribute("aria-pressed", String(kind === "serial"));
    $("usb-midi").setAttribute("aria-pressed", String(kind === "midi"));
    $("usb-off").hidden = !kind;
}
if (!inApp && !inPlugin && isSecureContext && (hasSerial() || hasMidi())) {
    document.querySelector<HTMLElement>(".usb")!.hidden = false;
    $("usb-serial").hidden = !hasSerial();
    $("usb-midi").hidden = !hasMidi();
    $("usb-serial").onclick = () => useUsb("serial", true);
    $("usb-midi").onclick = () => useUsb("midi", true);
    $("usb-off").onclick = stopUsb;
    const kind = localStorage.getItem(TRANSPORT_KEY);
    if (kind) useUsb(kind, false);
    if (hasSerial()) navigator.serial.addEventListener("connect", (e) => {
        if (localStorage.getItem(TRANSPORT_KEY) === "serial") link.use(serialSocket(e.target as SerialPort));
    });
    showUsb();
}

function showLoad(average: number, busiest: number, late: number) {
    $("load-fill").style.width = `${Math.min(100, average)}%`;
    $("load-peak").style.left = `${Math.min(99.5, busiest)}%`;
    $("load").textContent = `audio ${average.toFixed(0)}% · busiest ${busiest.toFixed(0)}%${late ? ` · ${late} late` : ""}`;
    document.querySelector("footer")!.classList.toggle("hot", busiest > 90 || late > 0);
}

const secs = (since: number) => Math.round((performance.now() - since) / 1000);
let downSince = 0, backUntil = 0, backAfter = 0;
function status() {
    const down = link.down, ever = link.everConnected;
    if (down && ever && !downSince) {
        downSince = link.lastReply;
        panel.clearKeys();  // the synth lets go of a lost page's notes
        playing.clear();
    }
    if (!down && downSince) {
        backAfter = secs(downSince);
        backUntil = performance.now() + 4000;
        downSince = 0;
    }
    const setLink = (text: string, kind: string) => {
        $("link").className = `link-${kind}`;
        $("link-text").textContent = text;
        // The panel says it too, for when it's shown alone (a phone held sideways).
        const reach = document.querySelector(".reach");
        if (reach) reach.innerHTML = `<span class="link-${kind}"><i class="dot"></i>${text}</span><span>${$("device").textContent}</span>`;
    };
    if (down && !ever) setLink(link.failures ? "not reached yet" : "connecting", "wait");
    else if (down) setLink(`no reply · ${secs(link.lastReply)} s`, "down");
    else if (performance.now() < backUntil) setLink(`back after ${backAfter} s`, "online");
    else setLink("online", "online");
    const via = link.transport;
    $("device").textContent = via ? `Pico over ${via} · ${down ? "not answering" : "48 kHz"}`
        : `${picoHost || location.host || "picoh-101"} · ${down ? "not answering" : "48 kHz"}`;
    document.body.classList.toggle("down", down && ever);
    document.querySelector("footer")!.classList.toggle("idle", down);

    const notice = $("notice");
    if (down && (ever || link.failures >= 3)) {
        const t = secs(downSince || performance.now());
        notice.className = "notice";
        $("notice-title").textContent = "Can’t reach the synth";
        if (inApp) {
            $("notice-title").textContent = "Plug in the Pico";
            $("notice-detail").textContent = "Connect it with a USB cable. The app finds it by itself, no need to do anything else.";
            notice.hidden = false;
            return;
        }
        if (localStorage.getItem(TRANSPORT_KEY)) {
            $("notice-title").textContent = "Can’t reach the Pico over USB";
            $("notice-detail").textContent = "Is it plugged in? Still trying, no need to reload.";
            notice.hidden = false;
            return;
        }
        $("notice-detail").textContent = (ever ? `No answer for ${t} s. ` : "") + "Still trying, no need to reload." +
            (t > 15 || !ever ? " Is this device still on the synth’s Wi-Fi? On its own network that is picoh-101, page at http://192.168.4.1/." : "");
        notice.hidden = false;
    } else if (performance.now() < backUntil) {
        notice.className = "notice back";
        $("notice-title").textContent = `Back after ${backAfter} s`;
        $("notice-detail").textContent = "Every control shows the synth’s settings again.";
        notice.hidden = false;
    } else notice.hidden = true;
}
if (inPlugin) {
    // Laid out once at the panel's 1180 x 644; the whole panel then scales with the window.
    const fit = () => { document.body.style.transform = `scale(${Math.min(innerWidth / 1180, innerHeight / 644)})`; };
    addEventListener("resize", fit);
    fit();
} else {
    $("notice-retry").onclick = () => link.reconnectNow();
    setInterval(status, 500);
    status();
}
