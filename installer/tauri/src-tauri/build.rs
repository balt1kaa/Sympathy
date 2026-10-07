fn main() {
    let manifest = if std::env::var("PROFILE").as_deref() == Ok("release") {
        include_str!("setup.manifest")
    } else {
        include_str!("setup.dev.manifest")
    };
    println!("cargo:rerun-if-changed=setup.manifest");
    println!("cargo:rerun-if-changed=setup.dev.manifest");
    let windows = tauri_build::WindowsAttributes::new().app_manifest(manifest);
    tauri_build::try_build(tauri_build::Attributes::new().windows_attributes(windows))
        .expect("tauri build failed");
}
