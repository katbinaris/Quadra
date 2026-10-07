// SYSTEM INFO: the knob's SYS INFO pages as four tiles, with a minute of history. Power is an
// estimate (the board has no current sense) -- labelled as such, like on the knob. This is the
// one page that needs the live stream (App turns it on while it's open).

import type { ComponentChildren } from "preact";
import { device, use } from "../store";
import { Box, PageHead } from "../ui/controls";

const USB_SOURCE = ["Reading", "PD chip not found", "USB", "USB-C", "USB-C", "USB PD"];
const amps = (ma: number) => `${(ma / 1000).toFixed(2)} A`;
const us = (v: number) => `${v.toFixed(1)} µs`;

export function SysPage() {
  use("sys");
  const a = device.sysA, b = device.sysB, h = device.history;
  return (
    <>
      <PageHead title="System info" hint="The last minute, twice a second. Power is an estimate: the board has no current sense.">
        <button class="btn ghost" onClick={() => device.resetPeaks()}>
          Reset peaks
        </button>
      </PageHead>
      {!a || !b ? (
        <Box>
          <span class="hint">Waiting for the knob…</span>
        </Box>
      ) : (
        <div class="grid2">
          <Tile
            title="Power"
            note="Estimated"
            big={amps(a.totalMa)}
            sub={a.usbMa > 0 && a.usbSource >= 2 ? `of ${amps(a.usbMa)} · ${(a.usbMv / 1000).toFixed(0)} V · ${USB_SOURCE[a.usbSource] ?? ""}` : (USB_SOURCE[a.usbSource] ?? "")}
            gauge={a.usbMa > 0 && a.usbSource >= 2 ? a.totalMa / a.usbMa : 0}
            peak={a.usbMa > 0 && a.usbSource >= 2 ? a.totalPeakMa / a.usbMa : undefined}
            warn={a.usbMa > 0 && a.totalMa > 0.8 * a.usbMa}
            history={h.totalMa}
            max={a.usbMa > 0 && a.usbSource >= 2 ? a.usbMa : Math.max(1000, ...h.totalMa)}
          >
            <KV k="Motor" v={amps(a.motorMa)} />
            <KV k="LEDs" v={amps(a.ledMa)} />
            <KV k="Board" v={amps(a.boardMa)} />
            <KV k="Peak" v={amps(a.totalPeakMa)} amber />
          </Tile>
          <Tile title="Heat" note="Chip" big={a.chipOk ? `${a.chipC.toFixed(1)} °C` : "—"} sub={a.chipOk ? `peak ${a.chipPeakC.toFixed(1)} °C` : "No sensor"} gauge={(a.chipC - 20) / 60} peak={(a.chipPeakC - 20) / 60} warn={a.chipC > 60} history={h.chipC} max={80}>
            <KV k="Coil" v={amps(a.coilMa)} />
            <KV k="Coil peak" v={amps(a.coilPeakMa)} amber />
            <KV k="Coil heat" v={`${a.copperW.toFixed(2)} W`} />
          </Tile>
          <Tile title="CPU" note="Load" big={`${b.load[0]}%  ${b.load[1]}%`} sub="core 0 the control loop · core 1 the rest" gauge={b.load[0] / 100} peak={b.loadPeak[0] / 100} history={h.load0} max={100}>
            <KV k="Loop" v={`${b.loopKhz.toFixed(2)} kHz`} />
            <KV k="Work" v={us(b.workAvgUs)} />
            <KV k="Work max" v={us(b.workMaxUs)} amber />
            <KV k="Jitter" v={us(b.jitterUs)} amber />
            <KV k="Spikes" v={`${b.spikesPerS.toFixed(0)}/s`} amber={b.spikesPerS >= 1} />
            <KV k="Missed" v={String(b.missed)} amber={b.missed > 0} />
          </Tile>
          <Tile
            title="System"
            note="Uptime"
            big={`${Math.floor(b.uptimeS / 3600)}:${String(Math.floor(b.uptimeS / 60) % 60).padStart(2, "0")}:${String(b.uptimeS % 60).padStart(2, "0")}`}
            sub="since the last restart"
            gauge={1 - b.heapFree / Math.max(b.heapFree, 320 * 1024)}
            history={h.load1}
            max={100}
          >
            <KV k="RAM free" v={`${Math.round(b.heapFree / 1024)}K`} />
            <KV k="RAM low" v={`${Math.round(b.heapMin / 1024)}K`} />
            <KV k="HID drops" v={String(b.hidDrops)} amber={b.hidDrops > 0} />
            <KV k="Sensor CRC errors" v={String(b.sensorCrcErrors)} amber={b.sensorCrcErrors > 0} />
          </Tile>
        </div>
      )}
    </>
  );
}

function KV(p: { k: string; v: string; amber?: boolean }) {
  return (
    <div class="kv">
      <span>{p.k}</span>
      <span class={p.amber ? "amber" : undefined}>{p.v}</span>
    </div>
  );
}

function Tile(p: { title: string; note: string; big: string; sub: string; gauge: number; peak?: number; warn?: boolean; history: number[]; max: number; children: ComponentChildren }) {
  const clamp = (x: number) => Math.max(0, Math.min(1, x)) * 100;
  const bars = p.history.slice(-120);
  return (
    <Box title={p.title} note={p.note} style={{ gap: "10px" }}>
      <div class="line" style={{ alignItems: "baseline", gap: "10px" }}>
        <span class="big">{p.big}</span>
        <span class="hint">{p.sub}</span>
      </div>
      <div class="gauge">
        <i style={{ width: `${clamp(p.gauge)}%`, background: p.warn ? "var(--amber)" : undefined }} />
        {p.peak !== undefined && <b style={{ left: `${clamp(p.peak)}%` }} />}
      </div>
      <div class="kvgrid two">{p.children}</div>
      <div class="spark" aria-hidden="true">
        {Array.from({ length: 120 - bars.length }, () => (
          <i style={{ height: "0%" }} />
        ))}
        {bars.map((v) => (
          <i style={{ height: `${clamp(v / (p.max || 1))}%` }} />
        ))}
      </div>
    </Box>
  );
}
