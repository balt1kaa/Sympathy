use serde::Serialize;

#[derive(Serialize, Clone)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Ev {
    Running { apps: Vec<String> },
    NoMediacore { path: String },
    Clearing,
    ClearFailed { path: String, error: String },
    Unpacking { size: String },
    File { name: String, size: String },
    Copying { name: String },
    Placing,
    Placed { path: String },
    UninstallerFailed { error: String },
    EntryPartial,
    Finished { target: String },
    Removing,
    RunningRemove { apps: Vec<String> },
    RemovalRunning,
    Failed { detail: String },
}

pub trait Reporter: Sync {
    fn ev(&self, e: Ev);
}

pub struct Silent;
impl Reporter for Silent {
    fn ev(&self, _: Ev) {}
}
