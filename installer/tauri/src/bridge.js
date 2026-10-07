const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

const params = new URLSearchParams(typeof location !== "undefined" ? location.search : "");

const SAMPLE = {
  mode: params.get("mode") === "uninstall" ? "uninstall" : "install",
  edition: params.get("problem")
    ? null
    : {
        prm: "Sympathy.prm",
        beside: false,
        files: ["avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "cineform license.txt", "ffmpeg license.txt", "ffmpeg notice.txt", "swresample-6.dll", "swscale-9.dll"],
      },
  problem: params.get("problem"),
  here: "C:\\Users\\name\\Downloads",
  source: "https://ashen.website/",
  mediacore: "C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore",
  target: "C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\Sympathy",
  uninstaller: "uninstall.exe",
  plugin: "C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\Sympathy",
  support: "C:\\Program Files\\Ashen One\\Sympathy",
};

const INSTALL = [
  { kind: "unpacking", size: "104.3 MB" },
  { kind: "file", name: "avcodec-62.dll", size: "82.4 MB" },
  { kind: "file", name: "avformat-62.dll", size: "15.1 MB" },
  { kind: "file", name: "avutil-60.dll", size: "2.6 MB" },
  { kind: "file", name: "cineform license.txt", size: "1.2 KB" },
  { kind: "file", name: "ffmpeg license.txt", size: "26.0 KB" },
  { kind: "file", name: "ffmpeg notice.txt", size: "1.0 KB" },
  { kind: "file", name: "swresample-6.dll", size: "420.0 KB" },
  { kind: "file", name: "swscale-9.dll", size: "740.0 KB" },
  { kind: "file", name: "Sympathy.prm", size: "1.6 MB" },
  { kind: "placing" },
  { kind: "placed", path: "C:\\Program Files\\Ashen One\\Sympathy\\uninstall.exe" },
  {
    kind: "finished",
    target: "C:\\Program Files\\Adobe\\Common\\Plug-ins\\7.0\\MediaCore\\Sympathy",
  },
];
const UNINSTALL = [{ kind: "removing" }, { kind: "removal_running" }];

export async function header() {
  if (!inTauri) return SAMPLE;
  const { invoke } = await import("@tauri-apps/api/core");
  return invoke("header");
}

export async function start(onEvent, onDone) {
  if (!inTauri) {
    const steps = SAMPLE.mode === "uninstall" ? UNINSTALL : INSTALL;
    let i = 0;
    const tick = () => {
      if (i < steps.length) {
        onEvent(steps[i++]);
        setTimeout(tick, 120);
      } else onDone(SAMPLE.mode !== "uninstall");
    };
    tick();
    return;
  }
  const { invoke } = await import("@tauri-apps/api/core");
  const { listen } = await import("@tauri-apps/api/event");
  await listen("ev", (e) => onEvent(e.payload));
  await listen("done", (e) => onDone(e.payload));
  await invoke("start");
}

export async function quit() {
  if (!inTauri) return;
  const { invoke } = await import("@tauri-apps/api/core");
  return invoke("quit");
}

export async function fit(width, height) {
  if (!inTauri) return;
  const { getCurrentWindow, PhysicalSize } = await import("@tauri-apps/api/window");
  const w = getCurrentWindow();
  const dpr = window.devicePixelRatio;
  await w.setSize(new PhysicalSize(Math.round(width * dpr), Math.round(height * dpr)));
  await w.center();
  await w.show();
}

export async function onArea(cb) {
  if (!inTauri) return cb(null);
  const { getCurrentWindow } = await import("@tauri-apps/api/window");
  const w = getCurrentWindow();
  const send = (s) => cb({ width: s.width / window.devicePixelRatio, height: s.height / window.devicePixelRatio });
  send(await w.innerSize());
  await w.onResized(({ payload }) => send(payload));
}

export async function drag() {
  if (!inTauri) return;
  const { getCurrentWindow } = await import("@tauri-apps/api/window");
  return getCurrentWindow().startDragging();
}
