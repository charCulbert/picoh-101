// The page is web/, as the browser and the Pico serve it: its TypeScript compiled (npm), then copied into
// dist/ for the app to carry.
use std::path::Path;
use std::process::Command;

// Left out of web/ itself: the TypeScript and its tooling.
const SKIP_TOP: &[&str] = &["src", "lib", "node_modules", "package.json", "package-lock.json", "tsconfig.json", "web.cmake"];
// Left out anywhere: what the vendored libraries carry beside their sources.
const SKIP: &[&str] = &["node_modules", "docs", "e2e", "examples", "test", "tests"];

fn npm(web: &Path, args: &[&str]) {
    let status = Command::new("npm").args(args).current_dir(web).status().expect("npm: install Node");
    assert!(status.success(), "npm {} failed in web/", args.join(" "));
}

fn copy(from: &Path, to: &Path, top: bool) {
    std::fs::create_dir_all(to).unwrap();
    for entry in std::fs::read_dir(from).unwrap() {
        let entry = entry.unwrap();
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') || SKIP.contains(&name.as_str()) || (top && SKIP_TOP.contains(&name.as_str())) {
            continue;
        }
        let (src, dst) = (entry.path(), to.join(&name));
        if src.is_dir() {
            copy(&src, &dst, false);
        } else {
            std::fs::copy(&src, &dst).unwrap();
        }
    }
}

// A fingerprint of every file's path, size and time, so the app (which embeds the page when it compiles)
// recompiles whenever the page changes: lib.rs reads it.
fn stamp(dir: &Path, hasher: &mut std::collections::hash_map::DefaultHasher) {
    use std::hash::Hash;
    for entry in std::fs::read_dir(dir).unwrap().flatten() {
        let meta = entry.metadata().unwrap();
        entry.path().hash(hasher);
        if meta.is_dir() {
            stamp(&entry.path(), hasher);
        } else {
            meta.len().hash(hasher);
            meta.modified().ok().hash(hasher);
        }
    }
}

fn main() {
    let web = Path::new("../web");
    let dist = Path::new("dist");
    if !web.join("node_modules/.package-lock.json").exists() {
        npm(web, &["ci", "--no-audit", "--no-fund"]);
    }
    npm(web, &["run", "build", "--silent"]);
    let _ = std::fs::remove_dir_all(dist);
    copy(web, dist, true);
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    stamp(dist, &mut hasher);
    println!("cargo:rustc-env=PICOH_PAGE_STAMP={}", std::hash::Hasher::finish(&hasher));
    println!("cargo:rerun-if-changed=../web/src");
    println!("cargo:rerun-if-changed=../web/package.json");
    println!("cargo:rerun-if-changed=../web/vendor/compost/src");
    println!("cargo:rerun-if-changed=../web/index.html");
    println!("cargo:rerun-if-changed=../web/instrument.css");
    println!("cargo:rerun-if-changed=../web/page.css");
    println!("cargo:rerun-if-changed=icons"); // generate_context! embeds them
    tauri_build::build()
}
