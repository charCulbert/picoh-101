// The app carries only the loader (loader/); the page comes from the Pico (src/page.rs). The page may call
// these commands and no others (capabilities/default.json).
fn main() {
    println!("cargo:rerun-if-changed=loader");
    println!("cargo:rerun-if-changed=icons"); // generate_context! embeds them
    let commands = tauri_build::AppManifest::new().commands(&["link_open", "link_send", "link_close", "debug_log"]);
    tauri_build::try_build(tauri_build::Attributes::new().app_manifest(commands)).expect("tauri-build")
}
