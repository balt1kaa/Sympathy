const breakable = (v) => (typeof v === "string" ? v.replaceAll("\\", "​\\") : v);
const WRAP = "whitespace-pre-wrap [overflow-wrap:anywhere]";

export function Row({ label = "", size, children }) {
  return (
    <>
      <div className="col-start-1 whitespace-nowrap">{label}</div>
      {size === undefined ? (
        <div className={`col-start-2 col-span-2 ${WRAP}`}>{[children].flat().map(breakable)}</div>
      ) : (
        <>
          <div className="col-start-2 whitespace-nowrap">{children}</div>
          <div className="col-start-3 whitespace-nowrap">{size}</div>
        </>
      )}
    </>
  );
}

export function Para({ children }) {
  return <div className={`col-span-3 ${WRAP}`}>{[children].flat().map(breakable)}</div>;
}

export function Blank() {
  return <div className="col-span-3">{" "}</div>;
}

export const DATE = "27 Sept 2026";

export const BUTTONS = {
  accept: "Accept",
  quit: "Close",
  close: "Close",
};

export function Closing() {
  return (
    <>
      <Blank />
      <Para>Sympathy</Para>
      <Para>Ashen One</Para>
    </>
  );
}

const threes = (list) => list.reduce((out, x, i) => (i % 3 ? out[out.length - 1].push(x) : out.push([x]), out), []);

export function Header({ h }) {
  if (h.mode === "uninstall") {
    return (
      <>
        <Row label="Rescind:">{h.plugin}</Row>
        <Row>{h.support}</Row>
      </>
    );
  }

  const ed = h.edition;
  return (
    <>
      {ed && <Row label="Edition:">Sympathy {DATE}</Row>}
      {ed && <Row label="Source:">{h.source}</Row>}
      <Row label="Target:">{h.mediacore}</Row>

      {ed && (
        <>
          <Row label="Manifest:">
            {ed.prm}
            {ed.beside && "  (beside setup.exe)"}
          </Row>
          {threes(ed.files).map((group, i) => (
            <Row key={i}>{group.join("  ")}</Row>
          ))}
          <Row>{h.uninstaller}</Row>
        </>
      )}

      {h.problem === "none" && (
        <>
          <Blank />
          <Para>No plugin file found.</Para>
          <Blank />
          <Para>This setup.exe carries none inside it, and there is no "Sympathy.prm" in:</Para>
          <Row>{h.here}</Row>
        </>
      )}
    </>
  );
}

export function Event({ e }) {
  switch (e.kind) {
    case "running":
      return (
        <>
          <Row label="Warning:">Running now: {e.apps.join(", ")}</Row>
          <Row>
            The plugin is picked up when the application next starts, and if it is holding the old file open this
            will fail. Closing it first is the safe order.
          </Row>
          <Blank />
        </>
      );
    case "no_mediacore":
      return (
        <>
          <Row label="Warning:">The Adobe MediaCore folder does not exist yet:</Row>
          <Row>{e.path}</Row>
          <Row>Creating it. If Premiere Pro is not installed, nothing will load it.</Row>
        </>
      );
    case "clearing":
      return <Para>Clearing the previous copy</Para>;
    case "clear_failed":
      return (
        <>
          <Row label="Failed:">
            Cannot clear {e.path}: {e.error}
          </Row>
          <Row>A file there is in use. Close Premiere Pro, After Effects and Media Encoder, then run this again.</Row>
        </>
      );
    case "unpacking":
      return (
        <>
          <Blank />
          <Para>Unpacking {e.size}</Para>
        </>
      );
    case "file":
      return <Row size={e.size}>{e.name}</Row>;
    case "copying":
      return (
        <>
          <Blank />
          <Para>Copying the plugin</Para>
          <Row>{e.name}</Row>
        </>
      );
    case "placing":
      return (
        <>
          <Blank />
          <Para>Placing the uninstaller</Para>
        </>
      );
    case "placed":
      return <Row>{e.path}</Row>;
    case "uninstaller_failed":
      return (
        <>
          <Row label="Warning:">Could not place the uninstaller: {e.error}</Row>
          <Row>The plugin is in place, but this will not appear in Settings &gt; Apps.</Row>
        </>
      );
    case "entry_partial":
      return <Para>Warning: the uninstall entry was written only in part</Para>;
    case "finished":
      return (
        <>
          <Blank />
          <Para>Sympathy is at:</Para>
          <Row>{e.target}</Row>
          <Blank />
          <Para>Premiere Pro, After Effects and Media Encoder load it at next start.</Para>
        </>
      );

    case "removing":
      return (
        <>
          <Para>Removing Sympathy.</Para>
          <Blank />
        </>
      );
    case "running_remove":
      return (
        <>
          <Row label="Warning:">Running now: {e.apps.join(", ")}</Row>
          <Row>Files held open by them cannot be deleted. Closing them first is the safe order.</Row>
          <Blank />
        </>
      );
    case "removal_running":
      return <Para>Removal is running. This window will close.</Para>;

    case "failed":
      return <Row label="Failed:">{e.detail}</Row>;
    default:
      return null;
  }
}
