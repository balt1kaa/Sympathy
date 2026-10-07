import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { drag, fit, header, onArea, quit, start } from "./bridge";
import { BUTTONS, Blank, Closing, Event, Header, Row } from "./Text";

const TOP = 14;
const SIDE = 28;
const BOTTOM = 18;
const GAP = 14;
const BTN_GAP = 32;
const LINES = 9;
const UNINSTALL_LINES = 4;
const LH = 1.2;
const SIZE = 12;

const snap = (px) => Math.round(px * window.devicePixelRatio) / window.devicePixelRatio;

const GRID = "grid grid-cols-[max-content_max-content_1fr] gap-x-[2ch]";
const BUTTON = "p-px m-0 border-0 bg-transparent text-ink font-grotesk cursor-pointer disabled:text-quiet disabled:cursor-default";

export default function App() {
  const [head, setHead] = useState(null);
  const [events, setEvents] = useState([]);
  const [state, setState] = useState("idle");
  const [area, setArea] = useState(null);
  const log = useRef(null);
  const probe = useRef(null);

  useEffect(() => {
    header().then(setHead);
    onArea(setArea);
  }, []);

  useLayoutEffect(() => {
    if (!head) return;
    document.fonts.ready.then(() => {
      const w = Math.ceil(probe.current.scrollWidth) + 2 * SIDE + 8;
      const lh = SIZE * LH;
      const lines = head.mode === "uninstall" ? UNINSTALL_LINES : LINES;
      const h = Math.ceil(TOP + lines * lh + GAP + (lh + 2) + BOTTOM);
      fit(w, h);
    });
  }, [head]);

  useEffect(() => {
    const el = log.current;
    if (el) el.scrollTop = state === "idle" ? 0 : el.scrollHeight;
  }, [events, state]);

  const ready = head && (head.mode === "uninstall" || head.edition);

  const go = () => {
    if (state !== "idle" || !ready) return;
    setState("running");
    start(
      (e) => setEvents((es) => [...es, e]),
      (ok) => {
        setState("done");
        if (head.mode === "uninstall" && ok) quit();
      },
    );
  };

  return (
    <div
      onMouseDown={(e) => e.button === 0 && !e.target.closest("button") && drag()}
      className={`${area ? "" : "h-full"} flex flex-col bg-ground text-ink font-grotesk antialiased select-none overflow-hidden cursor-default`}
      style={{ ...(area && { width: area.width, height: area.height }), fontSize: SIZE, lineHeight: LH, padding: `${TOP}px ${SIDE}px ${BOTTOM}px`, boxShadow: `inset 0 0 0 ${snap(1)}px var(--color-edge)`, borderRadius: snap(4) }}
    >
      <div ref={log} className={`no-bar flex-1 min-h-0 overflow-y-auto ${GRID} content-start`}>
        {head && <Header h={head} />}
        {events.length > 0 && <Blank />}
        {events.map((e, i) => (
          <Event key={i} e={e} />
        ))}
        {state === "done" && <Closing />}
      </div>

      <div className="flex justify-center" style={{ marginTop: GAP, columnGap: BTN_GAP }}>
        {state !== "done" && (
          <button type="button" disabled={state !== "idle" || !ready} onClick={go} className={BUTTON} style={{ fontSize: SIZE, lineHeight: LH }}>
            {BUTTONS.accept}
          </button>
        )}
        <button type="button" disabled={state === "running"} onClick={() => quit()} className={BUTTON} style={{ fontSize: SIZE, lineHeight: LH }}>
          {state === "done" ? BUTTONS.close : BUTTONS.quit}
        </button>
      </div>

      <div ref={probe} aria-hidden="true" className={`${GRID} fixed left-[-10000px] top-0 w-max [&_*]:!whitespace-nowrap`}>
        {head && (
          <>
            <Header h={head} />
            <Row>{head.target}</Row>
          </>
        )}
      </div>
    </div>
  );
}
