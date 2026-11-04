#!/usr/bin/env python3
"""Traces the Pico's audio core (firmware/trace.h): arms the ring over the USB serial port, waits for it to
freeze, reads it out, and writes a Chrome Trace Event file for Perfetto (https://ui.perfetto.dev: drag the
file in), with the control core's work on the same timeline. Prints the slowest blocks and what was in them.

usage: firmware/tools/trace.py over [percent] [post]   record until a block takes more than percent of its time
                                                         (default 80), then post blocks more (default 8)
       firmware/tools/trace.py blocks [n]               record the next n blocks (default 400)
options: --port /dev/cu.usbmodemX  --wait seconds (default 120)  --out file.json
         --play   play notes through the same port while it records (a note every 0.25 s, legato every
                  other one), for a test that repeats

Close the browser page's USB serial connection first: only one program can have the port. The trace is read
only after the ring froze, so reading it out doesn't disturb what it measured.
"""
import json
import os
import re
import select
import struct
import sys
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVENT_NAMES = {1: 'note on', 2: 'note off', 3: 'param', 4: 'sustain', 5: 'all notes off', 6: 'pitch bend', 7: 'mod wheel'}
CORE0_KINDS = {1: 'USB', 2: 'DIN MIDI', 3: 'Wi-Fi', 4: 'page line', 5: 'load report'}
STATES = ['idle', 'capturing', 'watching', 'triggered', 'frozen']
CAL, RENDER_BEGIN, RENDER_END, OUT_END, BLOCK_START, APPLY, BLOCK_END = 0x01, 0x02, 0x03, 0x04, 0x80, 0x81, 0x82


def dsp_points():
    """The synth's trace points (synth/DSP/picohDSP_TraceHook.h), from 0x10 in their order."""
    text = (ROOT / 'synth/DSP/picohDSP_TraceHook.h').read_text()
    names = re.findall(r'X \((\w+)\)', text.split('#define PICOH_TRACE_POINTS')[1].split('// Helpers')[0])
    return {0x10 + i: n for i, n in enumerate(names)}


def param_names():
    entries = json.loads((ROOT / 'params/parameters.json').read_text())['parameters']
    return {e['id']: e['key'] for e in entries}


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(self.fd)
        attrs[0] = attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.pending = b''

    def send(self, line):
        os.write(self.fd, (line + '\n').encode())

    def lines(self, timeout):
        """Trace lines ("# trace ..."), until timeout seconds pass without one."""
        end = time.time() + timeout
        while time.time() < end:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if ready:
                try:
                    self.pending += os.read(self.fd, 65536)
                except BlockingIOError:
                    pass
            while b'\n' in self.pending:
                raw, self.pending = self.pending.split(b'\n', 1)
                text = raw.decode(errors='replace').strip()
                if text.startswith('# trace '):
                    end = time.time() + timeout
                    yield text[len('# trace '):]


def status_of(line):
    f = line.split()
    return dict(state=STATES[int(f[1])], used=int(f[2]), blocks=int(f[3]), trigger=int(f[4]), dropped=int(f[5]),
                budget=int(f[6]), clock=int(f[7]), core0=int(f[8]))


def capture(port, command, wait, play=False):
    port.send('trace stop')
    port.send(command)
    print(f'armed: {command}' + (' (playing notes)' if play else ' (play, move knobs: whatever makes it slow)'))
    start = time.time()
    status = None
    notes = [48, 55, 60, 63, 67, 60, 55, 51]
    step = 0
    while time.time() - start < wait:
        if play:
            key = notes[step % len(notes)]
            port.send(f'n {key} 0.8')                 # the next note before the last one ends: legato
            if step % 2 == 0:
                port.send(f'n {notes[(step - 1) % len(notes)]} 0')
            else:
                time.sleep(0.05)
                port.send(f'n {notes[(step - 1) % len(notes)]} 0')
            step += 1
        port.send('trace status')
        for line in port.lines(0.6):
            if line.startswith('status'):
                status = status_of(line)
        if status and status['state'] == 'frozen':
            break
        if status:
            print(f"\r  {status['state']}: {status['blocks']} blocks", end='', flush=True)
    print()
    if play:
        port.send('a')
    if not status or status['state'] != 'frozen':
        port.send('trace stop')
        print('nothing slow enough in time; stopped the ring and reading what it has')
    data, spans = {}, []
    port.send('trace read')
    for line in port.lines(3):
        f = line.split()
        if f[0] == 'status':
            status = status_of(line)
        elif f[0] == 'data':
            data[int(f[1])] = bytes.fromhex(f[2])
        elif f[0] == 'core0':
            spans.append(tuple(int(x) for x in f[1:5]))
        elif f[0] == 'end':
            break
    raw = b''.join(data[k] for k in sorted(data))
    return status, raw, spans


def records(raw):
    at = 0
    while at + 8 <= len(raw):
        cycles, word = struct.unpack_from('<II', raw, at)
        point, a, b = word & 0xFF, (word >> 8) & 0xFF, word >> 16
        if point >= 0x80:
            x, y = struct.unpack_from('<II', raw, at + 8)
            at += 16
        else:
            x = y = 0
            at += 8
        yield cycles, point, a, b, x, y


def blocks_of(raw):
    block = None
    for r in records(raw):
        if r[1] == BLOCK_START:
            block = [r]
        elif block is not None:
            block.append(r)
            if r[1] == BLOCK_END:
                yield block
                block = None


def label(point, a, b, dsp, params):
    if point == APPLY:
        kind = EVENT_NAMES.get(a, f'event {a}')
        return f'{kind} {params.get(b, b)}' if a == 3 else f'{kind} {b}' if a in (1, 2) else kind
    return {CAL: 'stamp', RENDER_BEGIN: 'render', RENDER_END: 'render end', OUT_END: 'to I2S'}.get(point, dsp.get(point, hex(point)))


def main():
    args = sys.argv[1:]
    opts = {}
    play = '--play' in args
    if play:
        args.remove('--play')
    for name in ('--port', '--wait', '--out'):
        if name in args:
            i = args.index(name)
            opts[name] = args[i + 1]
            del args[i:i + 2]
    mode = args[0] if args else 'over'
    if mode == 'over':
        percent = float(args[1]) if len(args) > 1 else 80
        command = f'trace over {int(percent * 10)} {int(args[2]) if len(args) > 2 else 8}'
    elif mode == 'blocks':
        command = f'trace blocks {int(args[1]) if len(args) > 1 else 400}'
    else:
        sys.exit(__doc__)
    path = opts.get('--port') or next((f'/dev/{p}' for p in sorted(os.listdir('/dev')) if p.startswith('cu.usbmodem')), None)
    if not path:
        sys.exit('No Pico on USB (no /dev/cu.usbmodem*)')
    port = Port(path)
    status, raw, spans = capture(port, command, float(opts.get('--wait', 120)), play)
    if not raw:
        sys.exit('The ring is empty: no blocks recorded.')

    dsp, params = dsp_points(), param_names()
    mhz = status['clock'] / 1e6
    budget = status['budget']
    events, summary = [], []
    meta = [{'ph': 'M', 'pid': 1, 'name': 'process_name', 'args': {'name': f'Pico ({mhz:.0f} MHz, block {budget} cycles)'}},
            {'ph': 'M', 'pid': 1, 'tid': 1, 'name': 'thread_name', 'args': {'name': 'core 1: audio'}},
            {'ph': 'M', 'pid': 1, 'tid': 0, 'name': 'thread_name', 'args': {'name': 'core 0: USB, MIDI, Wi-Fi'}}]
    for block in blocks_of(raw):
        start_cycles, _, frames, waiting, block_us, number = block[0]
        end_cycles, _, pieces, applied, hits, accesses = block[-1]
        ts = lambda cycles: block_us + ((cycles - start_cycles) & 0xFFFFFFFF) / mhz
        cycles = (end_cycles - start_cycles) & 0xFFFFFFFF
        load = 100 * cycles / budget
        miss = 100 * (accesses - hits) / accesses if accesses else 0
        names = [label(r[1], r[2], r[3], dsp, params) for r in block if r[1] == APPLY]
        events.append({'ph': 'X', 'pid': 1, 'tid': 1, 'name': f'block {number}', 'ts': ts(start_cycles),
                       'dur': cycles / mhz, 'args': {'load %': round(load, 1), 'cycles': cycles, 'events': names,
                                                     'flash cache miss %': round(miss, 1), 'flash accesses': accesses,
                                                     'events waiting': waiting}})
        events.append({'ph': 'C', 'pid': 1, 'name': 'load %', 'ts': ts(start_cycles), 'args': {'load': round(load, 1)}})
        events.append({'ph': 'C', 'pid': 1, 'name': 'flash cache misses', 'ts': ts(start_cycles), 'args': {'misses': accesses - hits}})
        # Every stamp to the next is a slice, named by what it started (the render's own passes nest in it).
        inside = block[1:-1]
        for this, after in zip(inside, inside[1:] + [block[-1]]):
            if this[1] in (CAL, RENDER_END):
                continue
            events.append({'ph': 'X', 'pid': 1, 'tid': 1, 'name': label(this[1], this[2], this[3], dsp, params),
                           'ts': ts(this[0]), 'dur': ((after[0] - this[0]) & 0xFFFFFFFF) / mhz,
                           'args': {'a': this[2], 'b': this[3]} if this[1] < 0x80 else {'sample': this[5]}})
        summary.append((load, number, cycles, miss, accesses, names, block_us, block_us + cycles / mhz))
    for start_us, length_us, kind, arg in spans:
        events.append({'ph': 'X', 'pid': 1, 'tid': 0, 'name': CORE0_KINDS.get(kind, f'job {kind}'), 'ts': start_us, 'dur': length_us})

    out = Path(opts.get('--out') or ROOT / 'firmware/build/traces' / time.strftime('trace-%Y%m%d-%H%M%S.json'))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({'traceEvents': meta + events, 'displayTimeUnit': 'ns'}))

    loads = [s[0] for s in summary]
    print(f"{len(summary)} blocks, trigger block {status['trigger'] if status['trigger'] >= 0 else '-'}, "
          f"{status['dropped']} records dropped, {len(spans)} core 0 spans")
    print(f'load: average {sum(loads) / len(loads):.1f}%, busiest {max(loads):.1f}% '
          f'(a block has {budget} cycles, {budget / mhz:.0f} us)')
    slow = [s for s in summary if s[0] > 60]
    fast = [s for s in summary if s[0] <= 60]
    if slow and fast:
        avg = lambda xs, i: sum(x[i] for x in xs) / len(xs)
        print(f'flash cache misses: {avg(slow, 3):.1f}% in blocks over 60%, {avg(fast, 3):.1f}% in the rest')
    print('slowest blocks:')
    for load, number, cycles, miss, accesses, names, t0, t1 in sorted(summary, reverse=True)[:10]:
        busy = {}
        for start_us, length_us, kind, arg in spans:
            overlap = min(t1, start_us + length_us) - max(t0, start_us)
            if overlap > 0:
                busy[CORE0_KINDS.get(kind, kind)] = busy.get(CORE0_KINDS.get(kind, kind), 0) + overlap
        core0 = ', '.join(f'{k} {v:.0f} us' for k, v in busy.items()) or 'idle'
        print(f'  block {number:6}: {load:5.1f}% ({cycles} cycles), flash misses {miss:4.1f}% of {accesses}, '
              f"events: {', '.join(names) or '-'}; core 0: {core0}")
    print(f'\n{out}\nOpen it at https://ui.perfetto.dev (drag the file in).')


if __name__ == '__main__':
    main()
