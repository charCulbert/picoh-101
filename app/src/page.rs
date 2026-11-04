//! The page comes from the Pico (firmware/tools/embed.py makes it): over USB as SysEx (the `page` lines
//! of protocol/gui_lines.h), over Wi-Fi as GET /page. The app keeps the last one, in a folder named by its
//! hash, and serves it to the window in place of the loader it carries (loader/).
//!
//! The page is a block: a line "<size> <type> <path>" per file, an empty line, then the files. Nothing is
//! kept unless its FNV-1a 64 hash is the one the Pico gave and every path stays inside the folder.

use std::borrow::Cow;
use std::io::Read;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Mutex, OnceLock};
use tauri::utils::assets::{AssetKey, AssetsIter, CspHash};
use tauri::{Assets, Runtime};

static ROOT: OnceLock<PathBuf> = OnceLock::new();
static INSTALLING: Mutex<()> = Mutex::new(());

/// Where pages are kept: <root>/<hash>/ each with the block it came in as <root>/<hash>.block, and
/// <root>/current naming the one in use. The page kept is unpacked again from its block, checked; if
/// that fails it's dropped, to be fetched again.
pub fn set_root(root: PathBuf) {
    let _ = ROOT.set(root.clone());
    if let Some(hash) = current() {
        let read = |path: PathBuf| -> std::io::Result<Vec<u8>> {
            let mut block = vec![];
            std::fs::File::open(path)?.take(MAX_SIZE as u64 + 1).read_to_end(&mut block)?;
            Ok(block)
        };
        let unpacked = read(root.join(format!("{hash:016x}.block"))).map_err(|e| e.to_string()).and_then(|b| install(hash, &b, || true));
        if unpacked.is_err() {
            let _ = std::fs::remove_file(root.join("current"));
        }
    }
}

pub fn fnv1a64(data: &[u8]) -> u64 {
    data.iter().fold(0xcbf2_9ce4_8422_2325, |h, &b| (h ^ b as u64).wrapping_mul(0x100_0000_01b3))
}

pub fn current() -> Option<u64> {
    let root = ROOT.get()?;
    let hash = u64::from_str_radix(std::fs::read_to_string(root.join("current")).ok()?.trim(), 16).ok()?;
    root.join(format!("{hash:016x}")).join("index.html").is_file().then_some(hash)
}

fn safe(path: &str) -> Option<PathBuf> {
    let p = Path::new(path);
    (!path.is_empty() && p.components().all(|c| matches!(c, Component::Normal(_)))).then(|| p.to_path_buf())
}

/// The page's files, if the block is whole and is the one hashed.
fn unpack(hash: u64, block: &[u8]) -> Result<Vec<(PathBuf, &[u8])>, String> {
    if block.len() > MAX_SIZE {
        return Err("too big".into());
    }
    if fnv1a64(block) != hash {
        return Err("hash doesn't match".into());
    }
    let end = block.windows(2).position(|w| w == b"\n\n").ok_or("no header")? + 2;
    let header = std::str::from_utf8(&block[..end]).map_err(|_| "header isn't text")?;
    let mut files = vec![];
    let mut at = end;
    for line in header.lines().filter(|l| !l.is_empty()) {
        let mut words = line.splitn(3, ' ');
        let size: usize = words.next().and_then(|s| s.parse().ok()).ok_or("bad size")?;
        let path = words.nth(1).and_then(safe).ok_or_else(|| format!("bad path in {line:?}"))?;
        let next = at.checked_add(size).ok_or("bad size")?;
        files.push((path, block.get(at..next).ok_or("short")?));
        at = next;
    }
    if at != block.len() || !files.iter().any(|(p, _)| p == Path::new("index.html")) {
        return Err("files don't add up".into());
    }
    Ok(files)
}

/// Keeps the page and makes it current, if it's still wanted once it's whole on disk; the one before
/// stays until then. One install at a time.
pub fn install(hash: u64, block: &[u8], wanted: impl Fn() -> bool) -> Result<(), String> {
    static PARTS: AtomicU32 = AtomicU32::new(0);
    let files = unpack(hash, block)?;
    let root = ROOT.get().ok_or("no folder")?;
    let _one = INSTALLING.lock().unwrap_or_else(|e| e.into_inner());
    let name = format!("{hash:016x}");
    let part = root.join(format!("{name}.part{}", PARTS.fetch_add(1, Ordering::Relaxed)));
    let io = |e: std::io::Error| e.to_string();
    let unpacked = (|| {
        for (path, data) in files {
            let to = part.join(path);
            std::fs::create_dir_all(to.parent().unwrap())?;
            std::fs::write(to, data)?;
        }
        std::fs::write(root.join(format!("{name}.block.part")), block)
    })();
    if let Err(e) = unpacked {
        let _ = std::fs::remove_dir_all(&part);
        return Err(e.to_string());
    }
    if !wanted() {
        let _ = std::fs::remove_dir_all(&part);
        return Err("not the linked Pico's any more".into());
    }
    let _ = std::fs::remove_dir_all(root.join(&name));
    std::fs::rename(&part, root.join(&name)).map_err(io)?;
    std::fs::rename(root.join(format!("{name}.block.part")), root.join(format!("{name}.block"))).map_err(io)?;
    std::fs::write(root.join("current.part"), &name).map_err(io)?;
    std::fs::rename(root.join("current.part"), root.join("current")).map_err(io)?;
    let block_name = format!("{name}.block");
    for old in std::fs::read_dir(root).map_err(io)?.flatten() {
        let file = old.file_name();
        if file == name.as_str() || file == block_name.as_str() || file == "current" {
            continue;
        }
        let _ = if old.path().is_dir() { std::fs::remove_dir_all(old.path()) } else { std::fs::remove_file(old.path()) };
    }
    Ok(())
}

/// Returns once an install already running has finished.
pub fn wait_for_install() {
    drop(INSTALLING.lock().unwrap_or_else(|e| e.into_inner()));
}

/// What a fetch did: the page kept was the Pico's already, the Pico's replaced it, or neither.
#[derive(Debug, PartialEq)]
pub enum Fetched {
    Same,
    New,
    Failed(String),
}

const MAX_SIZE: usize = 1 << 21; // what the 21 bits of a chunk's offset reach (gui_lines.h)

/// Over USB: asks for the page's hash and size, and if it isn't the one kept, for the page, asking again
/// from the first byte missing whenever a chunk is skipped or the chunks stop.
#[cfg(target_vendor = "apple")]
pub fn fetch_usb(src: u32, dst: u32, mut progress: impl FnMut(usize, usize), wanted: impl Fn() -> bool) -> Fetched {
    use crate::midi;
    use std::sync::mpsc::{channel, RecvTimeoutError};
    use std::time::Duration;
    enum In {
        Line(String),
        Chunk(u32, Vec<u8>),
    }
    let (tx, rx) = channel();
    let lines = tx.clone();
    midi::on_page(Some(std::sync::Arc::new(move |offset, data| {
        let _ = tx.send(In::Chunk(offset, data));
    })));
    midi::listen(src, std::sync::Arc::new(move |line| {
        let _ = lines.send(In::Line(line));
    }));
    let result = (|| {
        midi::send(dst, "page");
        let (hash, size) = loop {
            match rx.recv_timeout(Duration::from_secs(2)) {
                Ok(In::Line(line)) => {
                    let mut words = line.split(' ');
                    if words.next() != Some("page") {
                        continue;
                    }
                    let hash = words.next().and_then(|h| u64::from_str_radix(h, 16).ok());
                    let size = words.next().and_then(|s| s.parse::<usize>().ok());
                    match (hash, size) {
                        (Some(h), Some(s)) if s <= MAX_SIZE => break (h, s),
                        _ => return Fetched::Failed(format!("bad answer {line:?}")),
                    }
                }
                Ok(In::Chunk(..)) => {}
                Err(_) => return Fetched::Failed("no answer to page".into()),
            }
        };
        if current() == Some(hash) {
            return Fetched::Same;
        }
        let mut block = Vec::with_capacity(size);
        let (mut asked, mut stalls) = (0, 0);
        midi::send(dst, "page 0");
        while block.len() < size {
            match rx.recv_timeout(Duration::from_millis(1000)) {
                Ok(In::Chunk(offset, data)) if offset as usize == block.len() => {
                    let take = data.len().min(size - block.len());
                    block.extend_from_slice(&data[..take]);
                    stalls = 0;
                    progress(block.len(), size);
                }
                Ok(In::Chunk(offset, _)) if offset as usize > block.len() && asked != block.len() => {
                    asked = block.len();
                    midi::send(dst, &format!("page {asked}"));
                }
                Ok(_) => {}
                Err(RecvTimeoutError::Timeout) if stalls < 5 => {
                    stalls += 1;
                    asked = block.len();
                    midi::send(dst, &format!("page {asked}"));
                }
                Err(_) => return Fetched::Failed(format!("stopped at {} of {size} bytes", block.len())),
            }
        }
        match install(hash, &block, wanted) {
            Ok(()) => Fetched::New,
            Err(e) => Fetched::Failed(e),
        }
    })();
    midi::on_page(None);
    midi::stop(src);
    result
}

/// Over Wi-Fi, from the Pico at address only (the one whose WebSocket is open): GET /page, with the kept
/// page's hash as If-None-Match.
pub fn fetch_http(address: std::net::SocketAddr, wanted: impl Fn() -> bool) -> Fetched {
    use std::io::Write;
    use std::time::Duration;
    let get = || -> Result<Vec<u8>, std::io::Error> {
        let mut stream = std::net::TcpStream::connect_timeout(&address, Duration::from_secs(2))?;
        stream.set_read_timeout(Some(Duration::from_secs(3)))?;
        let tag = current().map(|h| format!("If-None-Match: \"{h:016x}\"\r\n")).unwrap_or_default();
        stream.write_all(format!("GET /page HTTP/1.1\r\nHost: {}\r\n{tag}Connection: close\r\n\r\n", address.ip()).as_bytes())?;
        let mut reply = vec![];
        stream.take(MAX_SIZE as u64 + 4096).read_to_end(&mut reply)?; // the block and its headers
        Ok(reply)
    };
    let reply = match get() {
        Ok(r) => r,
        Err(e) => return Fetched::Failed(e.to_string()),
    };
    let Some(end) = reply.windows(4).position(|w| w == b"\r\n\r\n") else {
        return Fetched::Failed("no reply".into());
    };
    let head = String::from_utf8_lossy(&reply[..end]).to_ascii_lowercase();
    let status = head.split(' ').nth(1).unwrap_or("");
    if status == "304" {
        return Fetched::Same;
    }
    let hash = head.lines().find_map(|l| l.strip_prefix("etag:")).and_then(|v| u64::from_str_radix(v.trim().trim_matches('"'), 16).ok());
    match (status, hash) {
        ("200", Some(hash)) if current() == Some(hash) => Fetched::Same,
        ("200", Some(hash)) => match install(hash, &reply[end + 4..], wanted) {
            Ok(()) => Fetched::New,
            Err(e) => Fetched::Failed(e),
        },
        _ => Fetched::Failed(format!("status {status}")),
    }
}

/// A pageChunk message (protocol/gui_lines.h): its offset and bytes.
pub fn decode_chunk(m: &[u8]) -> Option<(u32, Vec<u8>)> {
    const ID: [u8; 4] = [0xF0, 0x7D, 0x50, 0x50];
    if m.len() < 8 || !m.starts_with(&ID) || m[m.len() - 1] != 0xF7 {
        return None;
    }
    let offset = (m[4] as u32) << 14 | (m[5] as u32) << 7 | m[6] as u32;
    let mut data = Vec::with_capacity(m.len());
    for group in m[7..m.len() - 1].chunks(8) {
        data.extend(group[1..].iter().enumerate().map(|(j, &b)| b | (group[0] >> j & 1) << 7));
    }
    Some((offset, data))
}

/// The window's files: the kept page if there is one, else the loader the app carries.
pub struct PageAssets<R: Runtime>(Option<Box<dyn Assets<R>>>);

pub fn use_kept_page<R: Runtime>(context: &mut tauri::Context<R>) {
    let loader = context.set_assets(Box::new(PageAssets(None)));
    context.set_assets(Box::new(PageAssets(Some(loader))));
}

impl<R: Runtime> Assets<R> for PageAssets<R> {
    fn get(&self, key: &AssetKey) -> Option<Cow<'_, [u8]>> {
        if let (Some(root), Some(hash)) = (ROOT.get(), current()) {
            let path = safe(key.as_ref().trim_start_matches('/'))?;
            return std::fs::read(root.join(format!("{hash:016x}")).join(path)).ok().map(Cow::Owned);
        }
        self.0.as_ref()?.get(key)
    }

    fn iter(&self) -> Box<AssetsIter<'_>> {
        match &self.0 {
            Some(loader) => loader.iter(),
            None => Box::new(std::iter::empty()),
        }
    }

    fn csp_hashes(&self, html_path: &AssetKey) -> Box<dyn Iterator<Item = CspHash<'_>> + '_> {
        match &self.0 {
            Some(loader) if current().is_none() => loader.csp_hashes(html_path),
            _ => Box::new(std::iter::empty()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunk_as_the_firmware_makes_it() {
        // firmware/tests/tests.cpp: bytes 255 down to 247 at 0x1ABCDE
        let m = [0xF0, 0x7D, 0x50, 0x50, 0x6A, 0x79, 0x5E, 0x7F, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x03, 0x78, 0x77, 0xF7];
        assert_eq!(decode_chunk(&m), Some((0x1ABCDE, (247..=255u8).rev().collect())));
        assert_eq!(decode_chunk(&m[..7]), None);
    }

    fn block(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut b: Vec<u8> = files.iter().flat_map(|(p, d)| format!("{} text/plain {p}\n", d.len()).into_bytes()).collect();
        b.push(b'\n');
        files.iter().for_each(|(_, d)| b.extend_from_slice(d));
        b
    }

    #[test]
    fn keeps_only_a_whole_page() {
        let dir = std::env::temp_dir().join(format!("picoh-page-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        set_root(dir.clone());
        let good = block(&[("index.html", b"<p>hi"), ("dist/app.js", b"x\n\ny")]);
        let hash = fnv1a64(&good);
        assert!(install(hash ^ 1, &good, || true).is_err());
        assert!(install(hash, &good[..good.len() - 1], || true).is_err());
        let evil = block(&[("index.html", b"a"), ("../x", b"b")]);
        assert!(install(fnv1a64(&evil), &evil, || true).is_err());
        let no_index = block(&[("a.html", b"a")]);
        assert!(install(fnv1a64(&no_index), &no_index, || true).is_err());
        let mut huge = format!("{} t index.html\n1 t a\n\n", usize::MAX).into_bytes();
        huge.push(b'x');
        assert!(install(fnv1a64(&huge), &huge, || true).is_err());
        assert!(install(hash, &good, || false).is_err());
        let mut too_big = format!("{} t index.html\n\n", MAX_SIZE).into_bytes();
        too_big.resize(too_big.len() + MAX_SIZE, b'x');
        assert!(install(fnv1a64(&too_big), &too_big, || true).is_err());
        assert_eq!(current(), None);
        install(hash, &good, || true).unwrap();
        assert_eq!(current(), Some(hash));
        assert_eq!(std::fs::read(dir.join(format!("{hash:016x}/dist/app.js"))).unwrap(), b"x\n\ny");
        let next = block(&[("index.html", b"<p>new")]);
        install(fnv1a64(&next), &next, || true).unwrap();
        assert_eq!(current(), Some(fnv1a64(&next)));
        assert!(!dir.join(format!("{hash:016x}")).exists() && !dir.join(format!("{hash:016x}.block")).exists());

        // At start: a damaged file is unpacked again from the block; a damaged block drops the page.
        let kept_dir = dir.join(format!("{:016x}", fnv1a64(&next)));
        std::fs::write(kept_dir.join("index.html"), b"<p>bad").unwrap();
        set_root(dir.clone());
        assert_eq!(std::fs::read(kept_dir.join("index.html")).unwrap(), b"<p>new");
        let block_file = dir.join(format!("{:016x}.block", fnv1a64(&next)));
        std::fs::write(&block_file, b"x").unwrap();
        set_root(dir.clone());
        assert_eq!(current(), None);
        install(fnv1a64(&next), &next, || true).unwrap();

        // Over HTTP from a fake Pico: a new page, then the same one (304), then one cut short.
        let kept = fnv1a64(&next);
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let fake = std::thread::spawn(move || {
            use std::io::{Read, Write};
            let mut asked = vec![];
            for reply in 0..3 {
                let (mut s, _) = listener.accept().unwrap();
                let (mut request, mut byte) = (vec![], [0u8]);
                while !request.ends_with(b"\r\n\r\n") && s.read(&mut byte).unwrap() == 1 {
                    request.push(byte[0]);
                }
                asked.push(String::from_utf8_lossy(&request).into_owned());
                let etag = format!("\"{hash:016x}\"");
                match reply {
                    0 => write!(s, "HTTP/1.1 200 OK\r\nContent-Length: {}\r\nETag: {etag}\r\n\r\n", good.len()).and_then(|_| s.write_all(&good)),
                    1 => write!(s, "HTTP/1.1 304 Not Modified\r\nETag: {etag}\r\n\r\n"),
                    _ => write!(s, "HTTP/1.1 200 OK\r\nETag: \"{:016x}\"\r\n\r\n", hash ^ 1).and_then(|_| s.write_all(&good[..good.len() - 3])),
                }
                .unwrap();
            }
            asked
        });
        assert_eq!(fetch_http(address, || true), Fetched::New);
        assert_eq!(current(), Some(hash));
        assert_eq!(fetch_http(address, || true), Fetched::Same);
        assert!(matches!(fetch_http(address, || true), Fetched::Failed(_)));
        assert_eq!(current(), Some(hash));
        let asked = fake.join().unwrap();
        assert!(asked[0].starts_with("GET /page ") && asked[0].contains(&format!("If-None-Match: \"{kept:016x}\"")));
        assert!(asked[1].contains(&format!("If-None-Match: \"{hash:016x}\"")));
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
