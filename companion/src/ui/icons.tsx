// A profile's icon at any size. With `id`, a sharper picture where this computer has one
// (appart.ts: the imported original, or a built-in's bundled artwork). Otherwise the knob's
// 48x48 pixels: whole multiples stay crisp (pixelated), smaller is a thumbnail, and black is
// see-through. No icon: its initial on a dark tile.

import { useEffect, useRef } from "preact/hooks";
import { artFor } from "../appart";
import { cls } from "./controls";

export function ProfileIcon(p: { icon: ImageData | null; name: string; size: number; ring?: boolean; id?: string; builtin?: boolean }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const art = p.id ? artFor(p.id, p.icon, !!p.builtin) : null;
  useEffect(() => {
    const c = ref.current;
    if (c && p.icon) c.getContext("2d")!.putImageData(p.icon, 0, 0);
  }, [p.icon, art]);
  const style = { width: `${p.size}px`, height: `${p.size}px` };
  if (art) return <img src={art} class={cls("picon", "art", p.ring && "inuse")} style={style} alt="" draggable={false} />;
  if (!p.icon)
    return (
      <span class={cls("picon", "blank", p.ring && "inuse")} style={{ ...style, fontSize: `${Math.round(p.size * 0.5)}px` }} aria-hidden="true">
        {p.name.slice(0, 1)}
      </span>
    );
  return <canvas ref={ref} width={48} height={48} class={cls("picon", p.size % 48 === 0 && "px", p.ring && "inuse")} style={style} aria-hidden="true" />;
}
