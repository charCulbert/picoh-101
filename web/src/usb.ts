// The link over the USB cable: the GUI lines over the Pico's serial port (Web Serial) or as SysEx on its
// MIDI port (Web MIDI). Each is a socket LiveView can open. Both need https or localhost.
import type { LineSocket, OpenSocket } from "./live.js";

// The Pico's USB identity (firmware/usb_descriptors.c): pid.codes' test pair.
export const USB_SERIAL: SerialPortFilter[] = [{ usbVendorId: 0x1209, usbProductId: 0x0001 }];
// The SysEx that carries a line: F0 7D 50 48 <line> F7 (0x7D: MIDI's non-commercial ID; 50 48: "PH").
const SYSEX_ID = [0xf0, 0x7d, 0x50, 0x48];
// The Pico's MIDI port, by name.
const MIDI_NAME = /picoh-101/i;

export const hasSerial = (): boolean => "serial" in navigator;
export const hasMidi = (): boolean => "requestMIDIAccess" in navigator;

function lineSocket(transport: string): LineSocket {
    return { readyState: 0, transport, onopen: null, onmessage: null, onclose: null, onerror: null, send() {}, close() {} };
}

// Over the serial port: a SerialPort from navigator.serial (requestPort, or getPorts for one already
// allowed). Lines end with \n; lines starting "# " are the Pico's logs, for people.
export function serialSocket(port: SerialPort): OpenSocket {
    return () => {
        const socket = lineSocket("USB serial");
        let writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
        let reader: ReadableStreamDefaultReader<string> | null = null;
        let closed = false;
        const finish = async () => {
            if (closed) return;
            closed = true;
            socket.readyState = 3;
            try { await reader?.cancel(); } catch {}
            try { writer?.releaseLock(); } catch {}
            try { await port.close(); } catch {}
            socket.onclose?.();
        };
        socket.send = (line) => {
            if (socket.readyState !== 1 || !writer) return;
            writer.write(new TextEncoder().encode(line + "\n")).catch(finish);
        };
        socket.close = () => { void finish(); };
        (async () => {
            try {
                // Opening raises DTR: the Pico sends every value.
                if (!port.readable) await port.open({ baudRate: 115200 });
                writer = port.writable!.getWriter();
                reader = port.readable!.pipeThrough(new TextDecoderStream() as unknown as ReadableWritablePair<string, Uint8Array>).getReader();
                socket.readyState = 1;
                socket.onopen?.();
                socket.send("hello");
                let pending = "";
                for (;;) {
                    const { value, done } = await reader.read();
                    if (done) break;
                    pending += value;
                    const lines = pending.split("\n");
                    pending = lines.pop() ?? "";
                    for (const line of lines) {
                        const text = line.replace(/\r$/, "");
                        if (text && !text.startsWith("#")) socket.onmessage?.({ data: text });
                    }
                }
            } catch (error) {
                console.warn("USB serial:", (error as Error).message);
            }
            void finish();
        })();
        // The Pico unplugged, or reset.
        navigator.serial.addEventListener("disconnect", (e) => { if (e.target === port) void finish(); });
        return socket;
    };
}

// Over SysEx on the MIDI port. MIDI has no open or close, so the page pings every 2 s; the Pico counts a
// page gone after 5 s of silence.
export function midiSocket(access: MIDIAccess): OpenSocket {
    return () => {
        const socket = lineSocket("USB MIDI");
        const find = <T extends MIDIPort>(ports: Map<string, T> | ReadonlyMap<string, T>) =>
            [...ports.values()].find((p) => MIDI_NAME.test(p.name ?? "") && p.state === "connected") ?? null;
        let input: MIDIInput | null = null, output: MIDIOutput | null = null, timer = 0;
        const finish = () => {
            if (socket.readyState === 3) return;
            socket.readyState = 3;
            clearInterval(timer);
            if (input) input.onmidimessage = null;
            socket.onclose?.();
        };
        const write = (line: string) => output?.send([...SYSEX_ID, ...[...line].map((c) => c.charCodeAt(0) & 0x7f), 0xf7]);
        socket.send = (line) => { if (socket.readyState === 1) write(line); };
        socket.close = finish;
        queueMicrotask(() => {
            input = find(access.inputs);
            output = find(access.outputs);
            if (!input || !output) return finish();
            input.onmidimessage = (e) => {
                const d = e.data;
                if (!d || d.length < SYSEX_ID.length + 1 || SYSEX_ID.some((b, i) => d[i] !== b)) return;
                socket.onmessage?.({ data: String.fromCharCode(...d.slice(SYSEX_ID.length, d.length - 1)) });
            };
            socket.readyState = 1;
            socket.onopen?.();
            write("hello");
            timer = setInterval(() => write("ping"), 2000);
        });
        access.addEventListener("statechange", (e) => {
            const port = (e as MIDIConnectionEvent).port;
            if (port?.state === "disconnected" && (port === input || port === output)) finish();
        });
        return socket;
    };
}
