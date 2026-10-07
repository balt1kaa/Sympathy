use crate::events::{Ev, Reporter};
use crate::payload::{extract_payload, has_payload, list_payload, write_uninstaller_copy};
use crate::util::*;
use serde::Serialize;
use std::path::{Path, PathBuf};
use winreg::enums::{HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, KEY_WRITE};
use winreg::RegKey;

pub struct Edition {
    pub prm_path: Option<PathBuf>,
    pub prm_name: String,
    pub display_name: String,
    pub embedded: bool,
    pub embedded_name: String,
}

#[derive(Serialize, Clone, Copy)]
#[serde(rename_all = "lowercase")]
pub enum Problem {
    None,
}

pub const PRM: &str = "Sympathy.prm";

pub fn find_edition() -> Result<Edition, Problem> {
    let here = here();
    let embedded_name = list_payload()
        .unwrap_or_default()
        .into_iter()
        .find(|n| n.eq_ignore_ascii_case(PRM))
        .unwrap_or_default();

    if here.join(PRM).is_file() {
        return Ok(Edition {
            prm_path: Some(here.join(PRM)),
            prm_name: PRM.into(),
            display_name: "Sympathy".into(),
            embedded: false,
            embedded_name,
        });
    }
    if !embedded_name.is_empty() {
        return Ok(Edition {
            prm_path: None,
            prm_name: embedded_name.clone(),
            display_name: "Sympathy".into(),
            embedded: true,
            embedded_name,
        });
    }
    Err(Problem::None)
}

fn dir_size(dir: &Path) -> u64 {
    let Ok(rd) = std::fs::read_dir(dir) else { return 0 };
    rd.flatten()
        .map(|e| match e.metadata() {
            Ok(m) if m.is_dir() => dir_size(&e.path()),
            Ok(m) => m.len(),
            Err(_) => 0,
        })
        .sum()
}

fn write_uninstall_entry(ed: &Edition, target: &Path, support: &Path, rep: &dyn Reporter) -> bool {
    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    let key = match hklm.create_subkey_with_flags(REG_UNINSTALL_KEY, KEY_WRITE | KEY_WOW64_64KEY) {
        Ok((k, _)) => k,
        Err(e) => {
            rep.ev(Ev::Failed { detail: format!("cannot write the uninstall entry: {}", err_text(&e)) });
            return false;
        }
    };
    let uninst = support.join(UNINSTALL_EXE);
    let quoted = format!("\"{}\" /uninstall", show(&uninst));
    let size_kb = ((dir_size(target) + dir_size(support)) / 1024) as u32;
    let results = [
        key.set_value("DisplayName", &ed.display_name),
        key.set_value("DisplayVersion", &PRODUCT_VERSION),
        key.set_value("Publisher", &PUBLISHER),
        key.set_value("InstallLocation", &show(target)),
        key.set_value("UninstallString", &quoted),
        key.set_value("QuietUninstallString", &format!("{quoted} /silent")),
        key.set_value("DisplayIcon", &format!("{},0", show(&uninst))),
        key.set_value("NoModify", &1u32),
        key.set_value("NoRepair", &1u32),
        key.set_value("EstimatedSize", &size_kb),
    ];
    let ok = results.iter().all(|r| r.is_ok());
    if !ok {
        rep.ev(Ev::EntryPartial);
    }
    ok
}

pub fn do_install(rep: &dyn Reporter) -> bool {
    let ed = match find_edition() {
        Ok(e) => e,
        Err(_) => {
            rep.ev(Ev::Failed { detail: "no plugin file to install".into() });
            return false;
        }
    };
    let mediacore = mediacore_dir();
    let target = target_dir();

    let Some(payload_bytes) = has_payload() else {
        rep.ev(Ev::Failed {
            detail: "this setup.exe carries no FFmpeg runtime. it was built but never packed -- run pack.exe over it.".into(),
        });
        return false;
    };

    let running = running_adobe_apps();
    if !running.is_empty() {
        rep.ev(Ev::Running { apps: running });
    }

    if !mediacore.is_dir() {
        rep.ev(Ev::NoMediacore { path: show(&mediacore) });
        if let Err(e) = ensure_dir(&mediacore) {
            rep.ev(Ev::Failed { detail: format!("cannot create {}: {}", show(&mediacore), err_text(&e)) });
            return false;
        }
    }

    if target.is_dir() {
        rep.ev(Ev::Clearing);
        if !remove_tree(&target) {
            rep.ev(Ev::ClearFailed { path: show(&target), error: err_text(&std::io::Error::last_os_error()) });
            return false;
        }
    }
    if let Err(e) = ensure_dir(&target) {
        rep.ev(Ev::Failed { detail: format!("cannot create {}: {}", show(&target), err_text(&e)) });
        return false;
    }

    let skip = if ed.embedded { String::new() } else { ed.embedded_name.clone() };

    rep.ev(Ev::Unpacking { size: format_bytes(payload_bytes) });
    if !extract_payload(&target, rep, &skip) {
        return false;
    }

    if let Some(src) = &ed.prm_path {
        if let Err(e) = std::fs::copy(src, target.join(&ed.prm_name)) {
            rep.ev(Ev::Failed { detail: format!("cannot copy {}: {}", ed.prm_name, err_text(&e)) });
            return false;
        }
        rep.ev(Ev::Copying { name: ed.prm_name.clone() });
    }

    let support = support_dir();
    rep.ev(Ev::Placing);
    if support.is_dir() {
        remove_tree(&support);
    }
    if let Err(e) = ensure_dir(&support) {
        rep.ev(Ev::UninstallerFailed { error: format!("cannot create {}: {}", show(&support), err_text(&e)) });
    } else if let Err(err) = write_uninstaller_copy(&support.join(UNINSTALL_EXE)) {
        rep.ev(Ev::UninstallerFailed { error: err });
    } else {
        rep.ev(Ev::Placed { path: show(&support.join(UNINSTALL_EXE)) });
        write_uninstall_entry(&ed, &target, &support, rep);
    }

    rep.ev(Ev::Finished {
        target: show(&target),
    });
    true
}
