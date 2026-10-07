use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub const MEDIACORE_RELATIVE: &str = r"Adobe\Common\Plug-ins\7.0\MediaCore";
pub const INSTALL_FOLDER: &str = "Sympathy";
pub const VENDOR_FOLDER: &str = "Ashen One";
pub const UNINSTALL_EXE: &str = "uninstall.exe";
pub const REG_UNINSTALL_KEY: &str = r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\sympathy";
pub const PRODUCT_VERSION: &str = "1.0.0";
pub const PUBLISHER: &str = "Ashen One";

pub const SOURCE_URL: &str = "https://ashen.website/";

pub const ADOBE_PROCESSES: [&str; 3] = ["Adobe Premiere Pro.exe", "AfterFX.exe", "Adobe Media Encoder.exe"];

pub fn exe_path() -> PathBuf {
    std::env::current_exe().unwrap_or_default()
}

pub fn here() -> PathBuf {
    exe_path().parent().map(Path::to_path_buf).unwrap_or_default()
}

pub fn show(p: &Path) -> String {
    p.display().to_string()
}

pub fn program_files() -> PathBuf {
    std::env::var_os("ProgramW6432")
        .or_else(|| std::env::var_os("ProgramFiles"))
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\Program Files"))
}

pub fn mediacore_dir() -> PathBuf {
    premiere_plugin_dir().unwrap_or_else(|| program_files().join(MEDIACORE_RELATIVE))
}

fn premiere_plugin_dir() -> Option<PathBuf> {
    use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_READ, KEY_WOW64_64KEY};
    let root = winreg::RegKey::predef(HKEY_LOCAL_MACHINE)
        .open_subkey_with_flags(r"SOFTWARE\Adobe\Premiere Pro", KEY_READ | KEY_WOW64_64KEY)
        .ok()?;
    let mut found: Vec<(f64, PathBuf)> = root
        .enum_keys()
        .flatten()
        .filter_map(|ver| {
            let n: f64 = ver.parse().ok()?;
            let path: String = root.open_subkey(&ver).ok()?.get_value("CommonPluginInstallPath").ok()?;
            let path = PathBuf::from(path.trim().trim_end_matches('\\'));
            path.is_absolute().then_some((n, path))
        })
        .collect();
    found.sort_by(|a, b| a.0.total_cmp(&b.0));
    found.pop().map(|(_, p)| p)
}

pub fn target_dir() -> PathBuf {
    mediacore_dir().join(INSTALL_FOLDER)
}

pub fn support_dir() -> PathBuf {
    program_files().join(VENDOR_FOLDER).join(INSTALL_FOLDER)
}

pub fn temp_dir() -> PathBuf {
    std::env::temp_dir()
}

pub fn err_text(e: &std::io::Error) -> String {
    let Some(code) = e.raw_os_error() else { return e.to_string() };
    let text = match code as u32 {
        2 => "The file was not found.",
        3 => "The path was not found.",
        5 => "Access is denied.",
        15 => "The drive was not found.",
        19 => "The disk is write-protected.",
        21 => "The device is not ready.",
        32 => "The file is in use by another program.",
        33 => "Part of the file is locked by another program.",
        39 | 112 => "There is not enough space on the disk.",
        80 | 183 => "The file already exists.",
        87 => "The parameter is incorrect.",
        123 => "The file name or folder name is not valid.",
        145 => "The folder is not empty.",
        161 => "The path is not valid.",
        206 => "The file name or path is too long.",
        225 => "The file contains a virus or unwanted software and was blocked.",
        1224 => "The file is open in another program.",
        1392 => "The file or folder is corrupted and unreadable.",
        _ => "Windows error.",
    };
    format!("{} (0x{:08X})", text, code as u32)
}

pub fn format_bytes(n: u64) -> String {
    const K: f64 = 1024.0;
    let f = n as f64;
    if n >= 1024 * 1024 * 1024 {
        format!("{:.2} GB", f / (K * K * K))
    } else if n >= 1024 * 1024 {
        format!("{:.1} MB", f / (K * K))
    } else if n >= 1024 {
        format!("{:.1} KB", f / K)
    } else {
        format!("{} bytes", n)
    }
}

pub fn crc32(data: &[u8], seed: u32) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let table = TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for i in 0..256u32 {
            let mut c = i;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            t[i as usize] = c;
        }
        t
    });
    let mut c = seed ^ 0xFFFF_FFFF;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFF_FFFF
}

pub fn remove_tree_ex(path: &Path, timeout: Duration) -> bool {
    if !path.is_dir() {
        return true;
    }
    let mut ok = true;
    if let Ok(entries) = std::fs::read_dir(path) {
        for entry in entries.flatten() {
            let child = entry.path();
            let Ok(meta) = std::fs::symlink_metadata(&child) else { ok = false; continue };
            let mut perms = meta.permissions();
            if perms.readonly() {
                #[allow(clippy::permissions_set_readonly_false)]
                perms.set_readonly(false);
                let _ = std::fs::set_permissions(&child, perms);
            }
            let ft = meta.file_type();
            if ft.is_symlink() || is_reparse(&meta) {
                if std::fs::remove_dir(&child).is_err() && std::fs::remove_file(&child).is_err() {
                    ok = false;
                }
            } else if ft.is_dir() {
                if !remove_tree_ex(&child, timeout) {
                    ok = false;
                }
            } else if std::fs::remove_file(&child).is_err() {
                ok = false;
            }
        }
    } else {
        return false;
    }
    let start = Instant::now();
    loop {
        match std::fs::remove_dir(path) {
            Ok(()) => return ok,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return ok,
            Err(_) => {
                if start.elapsed() >= timeout {
                    return false;
                }
                std::thread::sleep(Duration::from_millis(100));
            }
        }
    }
}

fn is_reparse(meta: &std::fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    meta.file_attributes() & 0x400 != 0
}

pub fn remove_tree(path: &Path) -> bool {
    remove_tree_ex(path, Duration::from_secs(4))
}

pub fn ensure_dir(path: &Path) -> std::io::Result<()> {
    let mut chain: Vec<&Path> = path.ancestors().take_while(|p| p.parent().is_some()).collect();
    chain.reverse();
    for dir in chain {
        if dir.is_dir() {
            continue;
        }
        if let Err(e) = std::fs::create_dir(dir) {
            if !dir.is_dir() {
                return Err(e);
            }
        }
    }
    Ok(())
}

pub fn running_adobe_apps() -> Vec<String> {
    use windows::Win32::Foundation::CloseHandle;
    use windows::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W, TH32CS_SNAPPROCESS,
    };
    let mut found: Vec<String> = Vec::new();
    unsafe {
        let Ok(snap) = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) else { return found };
        let mut pe = PROCESSENTRY32W { dwSize: std::mem::size_of::<PROCESSENTRY32W>() as u32, ..Default::default() };
        let mut more = Process32FirstW(snap, &mut pe).is_ok();
        while more {
            let len = pe.szExeFile.iter().position(|&c| c == 0).unwrap_or(pe.szExeFile.len());
            let name = String::from_utf16_lossy(&pe.szExeFile[..len]);
            for want in ADOBE_PROCESSES {
                if name.eq_ignore_ascii_case(want) && !found.iter().any(|f| f == want) {
                    found.push(want.to_string());
                }
            }
            more = Process32NextW(snap, &mut pe).is_ok();
        }
        let _ = CloseHandle(snap);
    }
    ADOBE_PROCESSES.iter().filter(|a| found.iter().any(|f| f == *a)).map(|a| a.to_string()).collect()
}

pub fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn delete_on_reboot(p: &Path) {
    use windows::core::PCWSTR;
    use windows::Win32::Storage::FileSystem::{MoveFileExW, MOVEFILE_DELAY_UNTIL_REBOOT};
    let w = wide(&show(p));
    unsafe {
        let _ = MoveFileExW(PCWSTR(w.as_ptr()), PCWSTR::null(), MOVEFILE_DELAY_UNTIL_REBOOT);
    }
}
