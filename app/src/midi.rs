//! The Pico's USB MIDI port through CoreMIDI (macOS and iOS): the GUI lines as SysEx
//! (F0 7D 50 48 <line> F7), both ways.

use core_foundation_sys::base::{CFRelease, CFTypeRef};
use core_foundation_sys::runloop::{kCFRunLoopDefaultMode, CFRunLoopRunInMode};
use core_foundation_sys::string::{kCFStringEncodingUTF8, CFStringCreateWithBytes, CFStringGetCString, CFStringRef};
use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};
use std::time::Duration;

type Ref = u32; // MIDIObjectRef

#[link(name = "CoreMIDI", kind = "framework")]
extern "C" {
    static kMIDIPropertyName: CFStringRef;
    static kMIDIPropertyOffline: CFStringRef;
    fn MIDIEndpointGetEntity(e: Ref, out: *mut Ref) -> i32;
    fn MIDIEntityGetDevice(e: Ref, out: *mut Ref) -> i32;
    fn MIDIClientCreate(name: CFStringRef, notify: Option<extern "C" fn(*const c_void, *mut c_void)>, refcon: *mut c_void, out: *mut Ref) -> i32;
    fn MIDIInputPortCreate(client: Ref, name: CFStringRef, read: extern "C" fn(*const u8, *mut c_void, *mut c_void), refcon: *mut c_void, out: *mut Ref) -> i32;
    fn MIDIOutputPortCreate(client: Ref, name: CFStringRef, out: *mut Ref) -> i32;
    fn MIDIGetNumberOfSources() -> usize;
    fn MIDIGetSource(i: usize) -> Ref;
    fn MIDIGetNumberOfDestinations() -> usize;
    fn MIDIGetDestination(i: usize) -> Ref;
    fn MIDIObjectGetStringProperty(obj: Ref, prop: CFStringRef, out: *mut CFStringRef) -> i32;
    fn MIDIObjectGetIntegerProperty(obj: Ref, prop: CFStringRef, out: *mut i32) -> i32;
    fn MIDIPortConnectSource(port: Ref, src: Ref, refcon: *mut c_void) -> i32;
    fn MIDIPortDisconnectSource(port: Ref, src: Ref) -> i32;
    fn MIDIPacketListInit(list: *mut u8) -> *mut u8;
    fn MIDIPacketListAdd(list: *mut u8, size: usize, cur: *mut u8, time: u64, n: usize, data: *const u8) -> *mut u8;
    fn MIDISend(port: Ref, dest: Ref, list: *const u8) -> i32;
}

/// The Pico's product name and pid.codes USB IDs (firmware/usb_descriptors.c)
const NAME: &str = "picoh-101";
const VID_PID: i32 = 0x1209_0001;
/// The SysEx that carries a line
const SYSEX_ID: [u8; 4] = [0xF0, 0x7D, 0x50, 0x48];

struct Ports {
    input: Ref,
    output: Ref,
}

/// Where the read callback puts what it gets: who wants the lines, and a SysEx message being gathered
struct Rx {
    on_line: Option<std::sync::Arc<dyn Fn(String) + Send + Sync>>,
    msg: Vec<u8>,
    inside: bool,
}

static RX: Mutex<Rx> = Mutex::new(Rx { on_line: None, msg: Vec::new(), inside: false });

fn cfstr(s: &str) -> CFStringRef {
    unsafe { CFStringCreateWithBytes(std::ptr::null(), s.as_ptr(), s.len() as _, kCFStringEncodingUTF8, 0) }
}

fn string_property(obj: Ref, prop: CFStringRef) -> Option<String> {
    let mut s: CFStringRef = std::ptr::null();
    unsafe {
        if MIDIObjectGetStringProperty(obj, prop, &mut s) != 0 || s.is_null() {
            return None;
        }
        let mut buf = [0i8; 256];
        let ok = CFStringGetCString(s, buf.as_mut_ptr(), buf.len() as _, kCFStringEncodingUTF8) != 0;
        CFRelease(s as CFTypeRef);
        ok.then(|| std::ffi::CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned())
    }
}

/// kMIDIPropertyUSBVendorProduct's value ("USBVendorProduct"; the symbol isn't exported everywhere)
fn usb_id_property() -> CFStringRef {
    static P: OnceLock<usize> = OnceLock::new();
    *P.get_or_init(|| cfstr("USBVendorProduct") as usize) as CFStringRef
}

/// An endpoint of the Pico: a real USB device (another app's virtual endpoint has no entity) with its
/// USB ID or name
fn is_pico(e: Ref) -> bool {
    let mut offline = 0;
    unsafe { MIDIObjectGetIntegerProperty(e, kMIDIPropertyOffline, &mut offline) };
    let named = |o, p| string_property(o, p).map(|n| n.to_ascii_lowercase().contains(NAME)).unwrap_or(false);
    let (mut entity, mut device, mut id) = (0, 0, 0);
    let usb = unsafe {
        MIDIEndpointGetEntity(e, &mut entity) == 0
            && entity != 0
            && MIDIEntityGetDevice(entity, &mut device) == 0
            && (MIDIObjectGetIntegerProperty(device, usb_id_property(), &mut id) == 0 && id == VID_PID
                || named(device, kMIDIPropertyName))
    };
    offline == 0 && usb
}

extern "C" fn read(list: *const u8, _: *mut c_void, _: *mut c_void) {
    // MIDIPacketList: numPackets u32, then packets (timeStamp u64, length u16, data), each packed to
    // 4 bytes on ARM (MIDIPacketNext)
    let mut rx = RX.lock().unwrap();
    let mut done = vec![];
    unsafe {
        let n = (list as *const u32).read_unaligned();
        let mut p = list.add(4);
        for _ in 0..n {
            let len = (p.add(8) as *const u16).read_unaligned() as usize;
            let data = std::slice::from_raw_parts(p.add(10), len);
            for &b in data {
                match b {
                    0xF0 => {
                        rx.msg.clear();
                        rx.msg.push(b);
                        rx.inside = true;
                    }
                    0xF7 if rx.inside => {
                        rx.msg.push(b);
                        rx.inside = false;
                        done.push(std::mem::take(&mut rx.msg));
                    }
                    0xF8..=0xFF => {} // real time, even inside SysEx
                    0x80..=0xF6 | 0xF7 => rx.inside = false,
                    _ if rx.inside && rx.msg.len() < 1024 => rx.msg.push(b),
                    _ => {}
                }
            }
            let next = p.add(10 + len) as usize;
            p = if cfg!(any(target_arch = "aarch64", target_arch = "arm")) { ((next + 3) & !3) as *const u8 } else { next as *const u8 };
        }
    }
    // Called without RX held: on_line takes the link's lock, which watch() holds while it calls listen and stop.
    let on_line = rx.on_line.clone();
    drop(rx);
    if let Some(on_line) = on_line {
        for m in done {
            if m.len() > SYSEX_ID.len() && m.starts_with(&SYSEX_ID) {
                on_line(String::from_utf8_lossy(&m[SYSEX_ID.len()..m.len() - 1]).into_owned());
            }
        }
    }
}

/// The client and its ports, made once on a thread that runs a run loop (CoreMIDI tells a client of
/// devices coming and going through it)
fn ports() -> Option<&'static Ports> {
    static PORTS: OnceLock<Option<Ports>> = OnceLock::new();
    PORTS
        .get_or_init(|| {
            let (tx, rx) = std::sync::mpsc::channel();
            std::thread::spawn(move || unsafe {
                extern "C" fn notify(_: *const c_void, _: *mut c_void) {}
                let (mut client, mut input, mut output) = (0, 0, 0);
                let ok = MIDIClientCreate(cfstr("PICOH-101 app"), Some(notify), std::ptr::null_mut(), &mut client) == 0
                    && MIDIInputPortCreate(client, cfstr("in"), read, std::ptr::null_mut(), &mut input) == 0
                    && MIDIOutputPortCreate(client, cfstr("out"), &mut output) == 0;
                let _ = tx.send(ok.then_some(Ports { input, output }));
                loop {
                    if CFRunLoopRunInMode(kCFRunLoopDefaultMode, 5.0, 0) != 3 {
                        std::thread::sleep(Duration::from_millis(500)); // nothing to run yet
                    }
                }
            });
            rx.recv().ok().flatten()
        })
        .as_ref()
}

/// The Pico's MIDI port, if it's plugged in: (source, destination)
pub fn find() -> Option<(Ref, Ref)> {
    ports()?;
    unsafe {
        let src = (0..MIDIGetNumberOfSources()).map(|i| MIDIGetSource(i)).find(|&e| is_pico(e))?;
        let dst = (0..MIDIGetNumberOfDestinations()).map(|i| MIDIGetDestination(i)).find(|&e| is_pico(e))?;
        Some((src, dst))
    }
}

/// Sends one line to the Pico as SysEx
pub fn send(dst: Ref, line: &str) -> bool {
    let Some(p) = ports() else { return false };
    let mut m = SYSEX_ID.to_vec();
    m.extend(line.bytes().map(|b| b & 0x7F));
    m.push(0xF7);
    let mut buf = vec![0u8; m.len() + 64];
    unsafe {
        let cur = MIDIPacketListInit(buf.as_mut_ptr());
        let cur = MIDIPacketListAdd(buf.as_mut_ptr(), buf.len(), cur, 0, m.len(), m.as_ptr());
        !cur.is_null() && MIDISend(p.output, dst, buf.as_ptr()) == 0
    }
}

/// Listens to the Pico's source: every line it sends goes to on_line, until stop
pub fn listen(src: Ref, on_line: std::sync::Arc<dyn Fn(String) + Send + Sync>) -> bool {
    let Some(p) = ports() else { return false };
    {
        let mut rx = RX.lock().unwrap();
        rx.on_line = Some(on_line);
        rx.inside = false;
    }
    unsafe { MIDIPortConnectSource(p.input, src, std::ptr::null_mut()) == 0 }
}

pub fn stop(src: Ref) {
    RX.lock().unwrap().on_line = None;
    if let Some(p) = ports() {
        unsafe { MIDIPortDisconnectSource(p.input, src) };
    }
}
