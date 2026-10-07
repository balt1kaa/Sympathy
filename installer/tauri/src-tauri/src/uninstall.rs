use crate::events::{Ev, Reporter};
use crate::payload::write_uninstaller_copy;
use crate::util::*;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_64KEY};
use winreg::RegKey;

pub fn installed_plugin_dir() -> PathBuf {
    RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(REG_UNINSTALL_KEY, KEY_READ | KEY_WOW64_64KEY)
        .and_then(|k| k.get_value::<String, _>("InstallLocation"))
        .ok()
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(target_dir)
}

pub fn do_uninstall(rep: &dyn Reporter) -> bool {
    use std::os::windows::process::CommandExt;
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    let support = here();
    rep.ev(Ev::Removing);

    let running = running_adobe_apps();
    if !running.is_empty() {
        rep.ev(Ev::RunningRemove { apps: running });
    }

    let pid = std::process::id();
    let temp_copy = temp_dir().join(format!("sympathy-uninstall-{pid}.exe"));
    if let Err(err) = write_uninstaller_copy(&temp_copy) {
        rep.ev(Ev::Failed { detail: format!("cannot stage the uninstaller in the temp folder: {err}") });
        return false;
    }
    let started = std::process::Command::new(&temp_copy)
        .arg("/cleanup")
        .arg(&support)
        .arg(pid.to_string())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn();
    if let Err(e) = started {
        rep.ev(Ev::Failed { detail: format!("cannot start the second stage: {}", err_text(&e)) });
        let _ = std::fs::remove_file(&temp_copy);
        return false;
    }
    rep.ev(Ev::RemovalRunning);
    true
}

fn wait_for(pid: u32, timeout_ms: u32) {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Threading::{OpenProcess, WaitForSingleObject, PROCESS_SYNCHRONIZE};
    unsafe {
        if let Ok(h) = OpenProcess(PROCESS_SYNCHRONIZE, false, pid) {
            WaitForSingleObject(h, timeout_ms);
            let _ = CloseHandle(h);
        }
    }
}

pub fn do_cleanup(support: &Path, parent_pid: u32) {
    if parent_pid != 0 {
        wait_for(parent_pid, 30_000);
        std::thread::sleep(Duration::from_millis(200));
    }

    let plugin = installed_plugin_dir();
    remove_tree_ex(&plugin, Duration::from_secs(60));
    remove_tree_ex(support, Duration::from_secs(60));

    for left in [plugin.as_path(), support] {
        if left.is_dir() {
            delete_on_reboot(left);
        }
    }

    if let Some(vendor) = support.parent() {
        let start = Instant::now();
        while vendor.is_dir() {
            if std::fs::remove_dir(vendor).is_ok() {
                break;
            }
            if !support.is_dir() && std::fs::read_dir(vendor).map(|mut d| d.next().is_some()).unwrap_or(false) {
                break;
            }
            if start.elapsed() >= Duration::from_secs(15) {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }

    let _ = RegKey::predef(HKEY_LOCAL_MACHINE).delete_subkey_with_flags(REG_UNINSTALL_KEY, KEY_WOW64_64KEY);

    delete_on_reboot(&exe_path());
}
