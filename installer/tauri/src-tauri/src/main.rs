#![windows_subsystem = "windows"]

mod events;
mod install;
mod payload;
mod uninstall;
mod util;

use events::{Ev, Reporter, Silent};
use serde::Serialize;
use std::sync::atomic::{AtomicBool, Ordering};
use tauri::{AppHandle, Emitter, Manager, WebviewUrl, WebviewWindowBuilder, WindowEvent};

struct Window(AppHandle);
impl Reporter for Window {
    fn ev(&self, e: Ev) {
        let _ = self.0.emit("ev", e);
    }
}

#[derive(Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
enum Mode {
    Install,
    Uninstall,
}

static MODE_UNINSTALL: AtomicBool = AtomicBool::new(false);
static RUNNING: AtomicBool = AtomicBool::new(false);
static SUCCEEDED: AtomicBool = AtomicBool::new(false);

fn mode() -> Mode {
    if MODE_UNINSTALL.load(Ordering::SeqCst) { Mode::Uninstall } else { Mode::Install }
}

#[derive(Serialize)]
struct EditionData {
    prm: String,
    beside: bool,
    files: Vec<String>,
}

#[derive(Serialize)]
struct Header {
    mode: Mode,
    edition: Option<EditionData>,
    problem: Option<install::Problem>,
    here: String,
    source: String,
    mediacore: String,
    target: String,
    uninstaller: String,
    plugin: String,
    support: String,
}

#[tauri::command]
fn header() -> Header {
    use util::*;
    let edition = install::find_edition();
    Header {
        mode: mode(),
        problem: edition.as_ref().err().copied(),
        edition: edition.ok().map(|ed| EditionData {
            files: payload::list_payload()
                .unwrap_or_default()
                .into_iter()
                .filter(|n| !n.eq_ignore_ascii_case(&ed.prm_name) && !n.eq_ignore_ascii_case(&ed.embedded_name))
                .collect(),
            prm: ed.prm_name,
            beside: !ed.embedded,
        }),
        here: show(&here()),
        source: SOURCE_URL.into(),
        mediacore: show(&mediacore_dir()),
        target: show(&target_dir()),
        uninstaller: UNINSTALL_EXE.into(),
        plugin: show(&uninstall::installed_plugin_dir()),
        support: show(&support_dir()),
    }
}

#[tauri::command]
fn start(app: AppHandle) {
    if RUNNING.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(move || {
        let rep = Window(app.clone());
        let ok = match mode() {
            Mode::Install => install::do_install(&rep),
            Mode::Uninstall => uninstall::do_uninstall(&rep),
        };
        SUCCEEDED.store(ok, Ordering::SeqCst);
        RUNNING.store(false, Ordering::SeqCst);
        let _ = app.emit("done", ok);
    });
}

#[tauri::command]
fn quit(app: AppHandle) {
    if !RUNNING.load(Ordering::SeqCst) {
        app.exit(if SUCCEEDED.load(Ordering::SeqCst) { 0 } else { 1 });
    }
}

fn window_title() -> &'static str {
    "Sympathy Setup"
}

fn flag(arg: &str, name: &str) -> bool {
    (arg.starts_with('/') || arg.starts_with('-')) && arg[1..].eq_ignore_ascii_case(name)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut want_uninstall = false;
    let mut silent = false;
    let mut cleanup: Option<(String, u32)> = None;
    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        if flag(a, "uninstall") {
            want_uninstall = true;
        } else if flag(a, "silent") || flag(a, "S") {
            silent = true;
        } else if flag(a, "cleanup") {
            let dir = args.get(i + 1).cloned().unwrap_or_default();
            let pid = args.get(i + 2).and_then(|p| p.parse().ok()).unwrap_or(0);
            cleanup = Some((dir, pid));
            i += 2;
        }
        i += 1;
    }

    let leaf = util::exe_path().file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    if leaf.eq_ignore_ascii_case(util::UNINSTALL_EXE) && cleanup.is_none() {
        want_uninstall = true;
    }

    if let Some((dir, pid)) = cleanup {
        if dir.is_empty() {
            std::process::exit(2);
        }
        uninstall::do_cleanup(std::path::Path::new(&dir), pid);
        std::process::exit(0);
    }

    if silent {
        let ok = if want_uninstall { uninstall::do_uninstall(&Silent) } else { install::do_install(&Silent) };
        std::process::exit(if ok { 0 } else { 1 });
    }

    MODE_UNINSTALL.store(want_uninstall, Ordering::SeqCst);

    let webview_data = util::temp_dir().join(format!("sympathy-setup-{}", std::process::id()));

    let app = tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![header, start, quit])
        .setup({
            let webview_data = webview_data.clone();
            move |app| {
                let window = WebviewWindowBuilder::new(app, "main", WebviewUrl::default())
                    .title(window_title())
                    .inner_size(620.0, 300.0)
                    .min_inner_size(240.0, 120.0)
                    .center()
                    .decorations(false)
                    .transparent(true)
                    .shadow(false)
                    .resizable(true)
                    .visible(false)
                    .data_directory(webview_data)
                    .build()?;
                square_corners(&window);
                Ok(())
            }
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                if RUNNING.load(Ordering::SeqCst) {
                    api.prevent_close();
                } else {
                    window.app_handle().exit(if SUCCEEDED.load(Ordering::SeqCst) { 0 } else { 1 });
                }
            }
        })
        .build({
            let mut ctx = tauri::generate_context!();
            ctx.set_default_window_icon(None);
            ctx
        })
        .expect("cannot start the window");

    app.run(|_, _| {});
    let _ = std::fs::remove_dir_all(&webview_data);
}

fn square_corners(window: &tauri::WebviewWindow) {
    use windows::Win32::Foundation::HWND;
    use windows::Win32::Graphics::Dwm::{
        DwmSetWindowAttribute, DWMWA_BORDER_COLOR, DWMWA_COLOR_NONE, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND,
    };
    let Ok(h) = window.hwnd() else { return };
    let hwnd = HWND(h.0 as *mut core::ffi::c_void);
    unsafe {
        let corner = DWMWCP_DONOTROUND;
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWA_WINDOW_CORNER_PREFERENCE,
            &corner as *const _ as *const core::ffi::c_void,
            std::mem::size_of_val(&corner) as u32,
        );
        let none = DWMWA_COLOR_NONE;
        let _ = DwmSetWindowAttribute(
            hwnd,
            DWMWA_BORDER_COLOR,
            &none as *const _ as *const core::ffi::c_void,
            std::mem::size_of_val(&none) as u32,
        );
    }
}
