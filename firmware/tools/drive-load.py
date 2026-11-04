#!/usr/bin/env python3
# Load test over the GUI serial port, far past what hands do: prints the Pico's "load" and "#" lines,
# phase by phase, then each phase's worst second.
# Usage: drive-load.py /dev/cu.usbmodemXXX [--minutes N: the phases again until N minutes have passed]
# The storm phases send 3 lines every 250 us (the port takes about 9,000 a second), so a block's events
# arrive at several samples: every slider and both wheels about every 2 ms, a switch every 40 ms, a note
# every 20 ms with legato overlaps, and every 100 ms a burst of five notes with attack and release in one
# write. Close the page's USB serial first. Each pass saves a sequence, stores patch 5 (key 45) 5 times,
# and stores patches 6 and 7 (keys 46, 47) to load them during a storm.
import os, sys, termios, time, math, select, random

port = sys.argv[1]
minutes = float(sys.argv[sys.argv.index('--minutes') + 1]) if '--minutes' in sys.argv else 0
perTick = 3
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[0] = 0; a[1] = 0; a[3] = 0; a[2] |= termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)
buf = b''
phase = ''
sent = 0
worst = {}  # per phase: highest average, busiest block, underruns, seconds, core 0's longest pass, late events,
           # the latest, dropped events

def pump():
    global buf
    while select.select([fd], [], [], 0)[0]:
        try: d = os.read(fd, 4096)
        except BlockingIOError: break
        if not d: break
        buf += d
    while b'\n' in buf:
        line, buf = buf.split(b'\n', 1)
        t = line.decode(errors='replace').strip()
        if t.startswith('load') or t.startswith('#'):
            print(f'{time.strftime("%H:%M:%S")} {phase:36s} {t}', flush=True)
        w = worst.setdefault(phase, [0.0, 0.0, 0, 0, 0, 0, 0, 0])
        if t.startswith('load '):
            average, busiest, underruns, loop, late, latest = (t.split()[1:7] + ['0'] * 6)[:6]
            w[0], w[1] = max(w[0], float(average)), max(w[1], float(busiest))
            w[2] += int(underruns); w[3] += 1
            w[4], w[5], w[6] = max(w[4], int(loop)), w[5] + int(late), max(w[6], int(latest))
        if t.startswith('#') and 'dropped' in t:
            w[7] += int(t.split()[1])

def send(*lines):
    global sent
    data = ''.join(l + '\n' for l in lines).encode()
    while data:
        try: data = data[os.write(fd, data):]
        except BlockingIOError: pump()
    sent += len(lines)

def run(seconds, tick=None, period=0.001):
    end = time.perf_counter() + seconds; k = 0; next_t = time.perf_counter()
    while time.perf_counter() < end:
        if tick: tick(k); k += 1
        next_t += period
        while time.perf_counter() < next_t: pump()

def log01(lo, hi, x): return lo * (hi / lo) ** x
s01 = lambda t, f, p=0.0: 0.5 + 0.5 * math.sin(2 * math.pi * (t * f + p))

# Every continuous control: (line prefix, value from t in seconds).
sliders = [
    ('v 0', lambda t: f'{-100 + 200 * s01(t, 0.7):.2f}'),
    ('v 1', lambda t: f'{log01(0.1, 30, s01(t, 0.31)):.3f}'),
    ('v 3', lambda t: f'{s01(t, 0.53):.3f}'),
    ('v 5', lambda t: f'{s01(t, 1.9):.3f}'),
    ('v 7', lambda t: f'{s01(t, 0.23, 0.1):.3f}'),
    ('v 8', lambda t: f'{s01(t, 0.29, 0.4):.3f}'),
    ('v 9', lambda t: f'{s01(t, 0.37, 0.7):.3f}'),
    ('v 11', lambda t: f'{0.3 * s01(t, 0.41):.3f}'),
    ('v 12', lambda t: f'{log01(20, 20000, s01(t, 0.9)):.1f}'),
    ('v 13', lambda t: f'{s01(t, 0.27) ** 0.3:.3f}'),  # mostly near self-oscillation
    ('v 14', lambda t: f'{s01(t, 0.61):.3f}'),
    ('v 15', lambda t: f'{s01(t, 0.43):.3f}'),
    ('v 16', lambda t: f'{s01(t, 0.19):.3f}'),
    ('v 19', lambda t: f'{log01(0.0015, 4, s01(t, 0.83)):.4f}'),
    ('v 20', lambda t: f'{log01(0.002, 10, s01(t, 0.71)):.4f}'),
    ('v 21', lambda t: f'{s01(t, 0.97):.3f}'),
    ('v 22', lambda t: f'{log01(0.002, 10, s01(t, 0.67)):.4f}'),
    ('v 23', lambda t: f'{0.4 + 0.4 * s01(t, 0.13):.3f}'),
    ('v 24', lambda t: f'{log01(0.001, 2, s01(t, 0.57)):.4f}'),
    ('v 26', lambda t: f'{s01(t, 0.33):.3f}'),
    ('v 27', lambda t: f'{s01(t, 0.39):.3f}'),
    ('v 28', lambda t: f'{s01(t, 0.47):.3f}'),
    ('b', lambda t: f'{math.sin(2 * math.pi * t * 1.1):.3f}'),
    ('m', lambda t: f'{s01(t, 0.77):.3f}'),
]
# Switches: (id, positions). Not arpeggio or hold, which phases set.
switches = [(2, 4), (4, 4), (6, 3), (10, 3), (17, 2), (18, 3), (25, 3), (29, 3)]
keys = [36, 48, 43, 60, 55, 72, 67, 84, 41, 53, 65, 77]

held = []
def storm(k, notes=True, pinned=(), keys=keys):
    t = k * 0.00025
    lines = []
    moving = [s for s in sliders if s[0] not in pinned]
    for j in range(perTick):
        prefix, value = moving[(k * perTick + j) % len(moving)]
        lines.append(f'{prefix} {value(t)}')
    if k % 160 == 0:
        free = [w for w in switches if f'v {w[0]}' not in pinned]
        sid, n = free[(k // 160) % len(free)]
        lines.append(f'v {sid} {(k // 1280) % n}')
    if notes and k % 80 == 0:  # a note every 20 ms, overlapping the last (legato), the oldest let go
        key = keys[(k // 80) % len(keys)]
        lines.append(f'n {key} {0.3 + 0.7 * s01(t, 2.3):.2f}'); held.append(key)
        if len(held) > 2: lines.append(f'n {held.pop(0)} 0')
    if notes and k % 400 == 200:  # the densest burst: five notes and two envelope times in one write
        for key in (48, 52, 55, 59, 62): lines.append(f'n {key} 0.9')
        lines += ['v 19 0.002', 'v 22 0.01'] + [f'n {key} 0' for key in (48, 52, 55, 59, 62)]
    send(*lines)

def notesOff():
    while held: send(f'n {held.pop()} 0')
    send('a')

start = time.time()
while True:
    phase = 'idle'; run(3)
    phase = 'default patch, C4 held'; send('n 60 1'); run(3)
    for pid, v in [(7, 1), (8, 1), (9, 1), (11, 0.2), (6, 2), (5, 0.7), (13, 0.95), (14, 0.8), (25, 2), (24, 0.15), (3, 0.1), (15, 0.3)]:
        send(f'v {pid} {v}')
    phase = 'heavy, notes every 100 ms'
    def notes(k):
        if k % 100 == 0:
            i = k // 100
            send(f'n {keys[(i - 1) % 8]} 0', f'n {keys[i % 8]} 1')
    run(4, notes)
    send('n 60 0', 'n 84 0')
    def storeEvery2s(k):  # a patch save into flash every 2 s, as the storm goes on
        storm(k)
        if k % 8000 == 4000: send('p store', 'n 45 1', 'n 45 0')
    phase = 'storm, STORE every 2 s'; sent = 0; run(10, storeEvery2s, 0.00025); notesOff()
    print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
    phase = 'storm + arpeggio'
    send('v 30 3', 'v 31 1'); [send(f'n {k} 1') for k in (48, 52, 55, 60, 64, 67)]
    sent = 0; run(10, lambda k: storm(k, notes=(k % 2 == 0)), 0.00025)
    print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
    send('v 31 0', 'v 30 0'); notesOff()
    phase = 'storm + sequencer'
    send('s load')
    for k in random.sample(range(36, 85), 16): send(f'n {k} 1', f'n {k} 0')  # a new one each run, so it saves
    send('s play')
    sent = 0; run(10, lambda k: storm(k, notes=False), 0.00025)
    print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
    send('s play'); notesOff()
    phase = 'storm, LFO 30 Hz on VCO + VCF'
    fast = ('v 1', 'v 2', 'v 3', 'v 15')
    send('v 1 30', 'v 3 1', 'v 15 1', 'v 2 1')
    sent = 0; run(5, lambda k: storm(k, pinned=fast), 0.00025); send('v 2 2'); run(5, lambda k: storm(k, pinned=fast), 0.00025)
    print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
    notesOff()
    send('v 1 3', 'v 2 0', 'v 3 0', 'v 15 0', 'b 0', 'm 0', 'v 23 0.7')
    # The highest notes, every source up, the pulse width swept as fast as it goes: by the LFO at 30 Hz, then by
    # an ADSR at its shortest times. RANGE 2' and TRANSPOSE up: the panel's top keys sound up to C8; MIDI's
    # top keys are held at the VCO's ceiling.
    top = ('v 1', 'v 2', 'v 4', 'v 5', 'v 6', 'v 7', 'v 8', 'v 9', 'v 11', 'v 19', 'v 20', 'v 21', 'v 22', 'v 29')
    for phase, highest in (("storm, all sources, PWM, keys to 72", range(61, 73)),
                           ("storm, all sources, PWM, keys to 127", range(116, 128))):
        send('v 4 3', 'v 29 2', 'v 7 1', 'v 8 1', 'v 9 1', 'v 11 1', 'v 5 1', 'v 1 30', 'v 2 0', 'v 6 0')
        sent = 0; run(5, lambda k: storm(k, pinned=top, keys=list(highest)), 0.00025)
        send('v 6 2', 'v 19 0.0015', 'v 20 0.002', 'v 21 0', 'v 22 0.002')
        run(5, lambda k: storm(k, pinned=top, keys=list(highest)), 0.00025)
        print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
        notesOff()
    # Patch LOADs under the storm: two patches with every source up and the pulse width swept (by the LFO,
    # then the ADSR), every value different, loaded in turn every 250 ms.
    worstA = ('v 4 3', 'v 29 2', 'v 7 1', 'v 8 1', 'v 9 1', 'v 11 1', 'v 5 1', 'v 1 30', 'v 2 0', 'v 6 0',
              'v 19 0.0015', 'v 20 0.002', 'v 21 0', 'v 22 0.002')
    worstB = ('v 7 0.9', 'v 8 0.9', 'v 9 0.9', 'v 11 0.9', 'v 5 0.9', 'v 1 25', 'v 6 2', 'v 19 0.002', 'v 20 0.003',
              'v 21 0.1', 'v 22 0.003')
    send(*worstA); send('p store', 'n 46 1', 'n 46 0'); run(0.2)
    send(*worstB); send('p store', 'n 47 1', 'n 47 0'); run(0.2)
    def loadEvery250ms(k):
        storm(k, pinned=top, keys=list(range(116, 128)))
        if k % 1000 == 500:
            key = 46 + (k // 1000) % 2
            send('p load', f'n {key} 1', f'n {key} 0')
    phase = 'storm, LOAD every 250 ms, keys to 127'
    sent = 0; run(10, loadEvery250ms, 0.00025)
    print(f'{phase:30s} # sent {sent / 10:.0f} lines a second', flush=True)
    notesOff()
    send('v 4 1', 'v 29 1', 'v 7 0', 'v 9 0', 'v 11 0', 'v 5 0', 'v 1 3', 'v 6 1', 'v 19 0.005', 'v 20 0.4',
         'v 21 0.5', 'v 22 0.3', 'v 23 0.7')
    phase = 'after, notes off'; run(3)
    if time.time() - start >= minutes * 60:
        break

print(f"\n{'phase':38s} {'avg':>6s} {'busiest':>8s} {'underruns':>9s} {'seconds':>7s} {'loop us':>7s} "
      f"{'late':>5s} {'latest us':>9s} {'dropped':>7s}")
for p, (average, busiest, underruns, seconds, loop, late, latest, dropped) in worst.items():
    print(f'{p:38s} {average:6.1f} {busiest:8.1f} {underruns:9d} {seconds:7d} {loop:7d} {late:5d} {latest:9d} {dropped:7d}')
