#!/usr/bin/env python3
"""Render a routed Vivado checkpoint as an interactive die map (one HTML file).

The page shows the device floorplan (every tile on the die, clock regions)
with the design's placed cells drawn over it, coloured by hierarchy.

Two steps, either of which can run alone:

    # 1. Ask Vivado for the floorplan and placement (needs Vivado):
    die_map.py dump build/foo_routed.dcp build/foo_die.tsv
    # 2. Render the dump (no Vivado; same dump -> same HTML):
    die_map.py render build/foo_die.tsv build/foo_die_map.html

    # Or both at once:
    die_map.py map build/foo_routed.dcp build/foo_die_map.html

Only the Python standard library is used.
"""

import argparse
from collections import Counter, defaultdict
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# The Tcl half lives next to this file; copy both to reuse them elsewhere.
TCL = Path(__file__).resolve().with_name("die_map.tcl")

# Tile categories: grid letter, legend label, CSS colour token. The order is
# the legend order.
FABRIC = [
    ("L", "Logic tile, 2 SLICEL", "clbll"),
    ("M", "Logic tile, SLICEM + SLICEL", "clblm"),
    ("B", "Block RAM tile (RAMB36)", "bram"),
    ("D", "DSP tile (2 DSP48E1)", "dsp"),
    ("I", "I/O tile", "io"),
    ("C", "Clocking (CMT, BUFG, HROW)", "clk"),
    ("G", "Transceiver (GTP)", "hard"),
    ("P", "PCIe hard block", "hard"),
    ("F", "Configuration / XADC", "hard"),
]
# Group colour tokens, assigned to hierarchy groups by cell count.
GROUP_TOKENS = 9


def tile_category(tile_type):
    """Grid letter for a tile type: one of FABRIC, 'n' (interconnect) or '.'."""
    t = tile_type
    if t.startswith("CLBLL"):
        return "L"
    if t.startswith("CLBLM"):
        return "M"
    if t in ("BRAM_L", "BRAM_R"):
        return "B"
    if t in ("DSP_L", "DSP_R"):
        return "D"
    if re.match(r"[LR]IO[BI]", t):
        return "I"
    if t.startswith(("CMT_TOP", "CLK_BUFG", "CLK_HROW")):
        return "C"
    if t.startswith("GTP_C"):
        return "G"
    if t.startswith("PCIE_") and not t.startswith("PCIE_INT") and t != "PCIE_NULL":
        return "P"
    if t.startswith(("CFG_CENTER", "MONITOR")):
        return "F"
    if t.startswith("INT_"):
        return "n"
    return "."


def primitive_class(ref):
    """Summary column for a primitive type."""
    if ref.startswith("LUT"):
        return "LUT"
    if ref.startswith("FD"):
        return "FF"
    if ref == "CARRY4":
        return "CARRY4"
    if ref.startswith("MUXF"):
        return "MUXF"
    if ref.startswith("RAMB") or ref.startswith("FIFO"):
        return "BRAM"
    if ref.startswith(("RAMD", "RAMS", "RAM32M", "RAM64M", "RAM32X", "RAM64X", "RAM128X", "RAM256X", "SRL")):
        return "LUTRAM"
    if ref.startswith("DSP"):
        return "DSP"
    return "other"


def group_of(path, depth):
    """Hierarchy group of a cell: its first `depth` instance names."""
    parents = path.split("/")[:-1]
    return "/".join(parents[:depth]) if parents else "(top level)"


def parse(text):
    """Parse a die_map.tcl dump into plain records."""
    d = {"version": "", "part": "", "top": "", "tiles": [], "regions": [], "cells": [], "pins": []}
    for n, line in enumerate(text.splitlines(), 1):
        if not line:
            continue
        f = line.split("\t")
        kind = f[0]
        try:
            if kind == "V":
                d["version"] = f[1]
            elif kind == "D":
                d["part"], d["top"] = f[1], f[2]
            elif kind == "T":
                d["tiles"].append((int(f[1]), int(f[2]), f[3]))
            elif kind == "R":
                d["regions"].append((f[1], *map(int, f[2:6])))
            elif kind == "C":
                d["cells"].append((int(f[1]), int(f[2]), f[3], f[4]))
            elif kind == "P":
                d["pins"].append((int(f[1]), int(f[2]), f[3], f[4]))
            else:
                raise ValueError(f"unknown record {kind!r}")
        except (IndexError, ValueError) as exc:
            raise ValueError(f"dump line {n}: {exc}: {line!r}") from None
    if not d["tiles"]:
        raise ValueError("dump has no tiles")
    return d


def model(d, depth):
    """The page's data: the grid, per-tile cell counts per group, summaries."""
    w = max(x for x, _, _ in d["tiles"]) + 1
    h = max(y for _, y, _ in d["tiles"]) + 1
    grid = [["."] * w for _ in range(h)]
    for x, y, t in d["tiles"]:
        grid[y][x] = tile_category(t)

    sizes = Counter(group_of(p, depth) for _, _, _, p in d["cells"])
    # Biggest groups get their own colour; the rest share "other".
    named = [g for g, _ in sorted(sizes.items(), key=lambda kv: (-kv[1], kv[0]))]
    if len(named) > GROUP_TOKENS:
        named = named[:GROUP_TOKENS - 1]
    lookup = {g: i for i, g in enumerate(named)}
    other = len(named) if len(sizes) > len(named) else None
    groups = named + (["(other)"] if other is not None else [])

    per_tile = defaultdict(Counter)
    summary = [Counter() for _ in groups]
    for x, y, ref, path in d["cells"]:
        g = lookup.get(group_of(path, depth), other)
        per_tile[(x, y)][g] += 1
        summary[g][primitive_class(ref)] += 1
    cells = [[x, y, g, n] for (x, y), c in sorted(per_tile.items()) for g, n in sorted(c.items())]

    tiles = Counter(c for row in grid for c in row)
    return {
        "w": w, "h": h,
        "grid": ["".join(r) for r in grid],
        "regions": [list(r) for r in sorted(d["regions"])],
        "pins": [list(p) for p in sorted(d["pins"])],
        "groups": groups,
        "cells": cells,
        "summary": [dict(sorted(s.items())) for s in summary],
        "fabric": [[k, label, token, tiles.get(k, 0)] for k, label, token in FABRIC],
        "slices": 2 * (tiles.get("L", 0) + tiles.get("M", 0)),
        "placed": len(d["cells"]),
        "part": d["part"], "top": d["top"], "version": d["version"], "depth": depth,
    }


def render(d, depth=2, title=None):
    """The HTML page for a parsed dump."""
    m = model(d, depth)
    title = title or f"{m['top']} on {m['part']}"
    data = json.dumps(m, separators=(",", ":"), sort_keys=True).replace("</", "<\\/")
    esc = lambda s: s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return (PAGE.replace("@TITLE@", esc(title))
                .replace("@PART@", esc(m["part"]))
                .replace("@TOP@", esc(m["top"]))
                .replace("@VERSION@", esc(m["version"]))
                .replace("@DATA@", data))


def dump(checkpoint, out, vivado):
    """Run Vivado on `checkpoint` and write the dump to `out`."""
    checkpoint, out = Path(checkpoint).resolve(), Path(out).resolve()
    if not checkpoint.is_file():
        raise FileNotFoundError(f"checkpoint does not exist: {checkpoint}")
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        # Vivado leaves logs and .Xil/ in its working directory.
        subprocess.run([vivado, "-mode", "batch", "-nolog", "-nojournal", "-source", str(TCL),
                        "-tclargs", str(checkpoint), str(out)], cwd=tmp, check=True)
    if not out.is_file():
        raise RuntimeError(f"Vivado did not write {out}")


def write_page(dump_path, out, depth, title):
    d = parse(Path(dump_path).read_text())
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(render(d, depth, title))
    print(f"Wrote {out} ({len(d['cells'])} placed cells, {len(d['tiles'])} tiles)", flush=True)


def default_vivado():
    root = Path(os.environ.get("XILINX_ROOT", Path.home() / "Xilinx/2025.2"))
    return str(root / "Vivado" / "bin" / "vivado")


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = cli.add_subparsers(dest="command", required=True)
    p_dump = sub.add_parser("dump", help="extract floorplan and placement with Vivado")
    p_render = sub.add_parser("render", help="render a dump to HTML (no Vivado)")
    p_map = sub.add_parser("map", help="dump and render in one go")
    for p in (p_dump, p_map):
        p.add_argument("checkpoint", type=Path, help="routed (or placed) .dcp")
        p.add_argument("--vivado", default=default_vivado(), help="vivado executable")
    p_render.add_argument("dump", type=Path)
    p_dump.add_argument("out", type=Path, help="dump file to write (.tsv)")
    for p in (p_render, p_map):
        p.add_argument("out", type=Path, help="HTML file to write")
        p.add_argument("--depth", type=int, default=2,
                       help="hierarchy levels used to group cells (default 2)")
        p.add_argument("--title", help="page title (default '<top> on <part>')")
    p_map.add_argument("--keep-dump", type=Path, help="also keep the dump at this path")
    args = cli.parse_args(argv)
    if getattr(args, "depth", 1) < 1:
        cli.error("--depth must be at least 1")
    try:
        if args.command == "dump":
            dump(args.checkpoint, args.out, args.vivado)
        elif args.command == "render":
            write_page(args.dump, args.out, args.depth, args.title)
        else:
            with tempfile.TemporaryDirectory() as tmp:
                tsv = args.keep_dump or Path(tmp) / "die.tsv"
                dump(args.checkpoint, tsv, args.vivado)
                write_page(tsv, args.out, args.depth, args.title)
    except subprocess.CalledProcessError as exc:
        print(f"Vivado failed with exit status {exc.returncode}", file=sys.stderr)
        return 1
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    return 0


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>@TITLE@</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Space+Grotesk:wght@500;700&family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* Layout: die map as the hero on the left, legend and readouts in a narrow right column; stacks on phones. */
:root{
  --bg:#eef0ec; --panel:#f8f9f6; --fg:#1d2420; --muted:#5d6a63; --line:#cfd6d0; --substrate:#dde2dc;
  --int:#d2d8d2; --clbll:#9fb0a6; --clblm:#7f978a; --bram:#d39b2a; --dsp:#8a5cc2; --io:#2a8f8a; --clk:#d0496a; --hard:#a7a39a;
  --g0:#2f6fd6; --g1:#0d9b6c; --g2:#e0631e; --g3:#c99a00; --g4:#b8379a; --g5:#178fb0; --g6:#7a5c2e; --g7:#c2342d; --g8:#5a5f66;
  --display:"Space Grotesk",system-ui,sans-serif; --body:"IBM Plex Sans",system-ui,sans-serif; --mono:"IBM Plex Mono",ui-monospace,monospace;
  color-scheme:light;
}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){
  --bg:#121614; --panel:#1a201d; --fg:#e4ebe6; --muted:#93a39a; --line:#2c3531; --substrate:#1c2320;
  --int:#232b27; --clbll:#3c4a43; --clblm:#4d5f55; --bram:#c08a1e; --dsp:#9b72d6; --io:#2fa8a1; --clk:#e05b7c; --hard:#5c5a55;
  --g0:#5b93f0; --g1:#2fc48f; --g2:#f58442; --g3:#e8bd2c; --g4:#dc5cbf; --g5:#3cbde0; --g6:#c49a5a; --g7:#f0605a; --g8:#b8bec6; color-scheme:dark}}
:root[data-theme="dark"]{
  --bg:#121614; --panel:#1a201d; --fg:#e4ebe6; --muted:#93a39a; --line:#2c3531; --substrate:#1c2320;
  --int:#232b27; --clbll:#3c4a43; --clblm:#4d5f55; --bram:#c08a1e; --dsp:#9b72d6; --io:#2fa8a1; --clk:#e05b7c; --hard:#5c5a55;
  --g0:#5b93f0; --g1:#2fc48f; --g2:#f58442; --g3:#e8bd2c; --g4:#dc5cbf; --g5:#3cbde0; --g6:#c49a5a; --g7:#f0605a; --g8:#b8bec6; color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 var(--body)}
.wrap{max-width:1180px;margin:0 auto;padding-block:28px 48px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
h1{font-family:var(--display);font-size:1.9rem;margin:0;letter-spacing:-.01em;text-wrap:balance}
h2{font-family:var(--display);font-size:1.05rem;margin:0 0 8px}
.sub{color:var(--muted);max-width:70ch;margin:6px 0 20px}
.mono{font-family:var(--mono);font-variant-numeric:tabular-nums}
.grid{display:grid;grid-template-columns:minmax(0,1fr) 360px;gap:24px;align-items:start}
@media (max-width:880px){.grid{grid-template-columns:minmax(0,1fr)}}
.stage{position:relative;background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:12px;min-width:0}
canvas{display:block;width:100%;height:auto;cursor:crosshair}
.bar{display:flex;flex-wrap:wrap;gap:6px;margin-bottom:10px}
.bar[hidden]{display:none}
.bar button{font:500 .78rem var(--mono);background:transparent;color:var(--fg);border:1px solid var(--line);border-radius:4px;padding:4px 9px;cursor:pointer;display:flex;align-items:center;gap:6px;max-width:100%;overflow-wrap:anywhere;text-align:left}
.bar button[aria-pressed="true"]{background:var(--fg);color:var(--bg);border-color:var(--fg)}
.bar button:focus-visible{outline:2px solid var(--g0);outline-offset:2px}
.sw{width:10px;height:10px;border-radius:2px;display:inline-block;flex:none}
#tip{position:absolute;pointer-events:none;background:var(--fg);color:var(--bg);font:12px/1.4 var(--mono);padding:6px 8px;border-radius:4px;white-space:pre;display:none;z-index:2}
aside{display:flex;flex-direction:column;gap:22px;min-width:0}
.scroll{overflow-x:auto}
table{border-collapse:collapse;width:100%;font-size:.84rem}
td,th{padding:4px 6px;border-bottom:1px solid var(--line);text-align:left;vertical-align:top}
th{font-weight:600;color:var(--muted);font-size:.7rem;text-transform:uppercase;letter-spacing:.06em}
td.n{text-align:right;font-family:var(--mono);font-variant-numeric:tabular-nums;white-space:nowrap}
td.g{font-family:var(--mono);font-size:.78rem;overflow-wrap:anywhere}
.note{font-size:.85rem;color:var(--muted)}
.note b{color:var(--fg)}
footer{margin-top:28px;font-size:.78rem;color:var(--muted)}
</style>
</head>
<body>
<div class="wrap">
<h1>@TITLE@</h1>
<p class="sub">Floorplan of <span class="mono">@PART@</span> from Vivado's device database, with the placed cells of <span class="mono">@TOP@</span> drawn over it, coloured by hierarchy. The top of the map is the top of the die. Hover over a tile to inspect it.</p>
<div class="grid">
<div class="stage">
<div class="bar" role="group" aria-label="Show">
<button id="b-fabric" type="button" aria-pressed="false">Fabric only</button>
<button id="b-design" type="button" aria-pressed="true">Placed design</button>
</div>
<div class="bar" id="groups" role="group" aria-label="Highlight one group"></div>
<canvas id="die" aria-label="Die floorplan with placed cells"></canvas>
<div id="tip"></div>
</div>
<aside>
<section><h2>Fabric</h2><div class="scroll"><table id="fab"></table></div></section>
<section><h2>Placed cells</h2><div class="scroll"><table id="use"></table></div></section>
<section class="note">
<p><b>Columns.</b> 7-series silicon is built from vertical columns: logic, with a block RAM or DSP column every few columns and I/O banks at the edges. Carry chains run upward inside one logic column, 4 bits per slice.</p>
<p><b>Clock regions</b> are outlined. Global clock buffers sit in the centre spine.</p>
<p><b>Grey blocks</b> are transceivers, PCIe and configuration logic. Whether they are usable depends on the package: the CSG324 package doesn't bond the GTP pins.</p>
</section>
</aside>
</div>
<footer class="mono" id="foot"></footer>
</div>
<script>
const DIE=@DATA@;
(function(){
const S=5,W=DIE.w,H=DIE.h,cv=document.getElementById('die'),cx=cv.getContext('2d'),tip=document.getElementById('tip');
cv.width=W*S;cv.height=H*S;
const css=n=>getComputedStyle(document.documentElement).getPropertyValue('--'+n).trim();
const gtok=g=>'g'+Math.min(g,8);
let showDesign=true,focus=null;
const byTile=new Map();
for(const [x,y,g,n] of DIE.cells){const k=x+','+y;if(!byTile.has(k))byTile.set(k,{});byTile.get(k)[g]=n}
const tall=c=>c==='B'||c==='D';
function draw(){
  const col={n:css('int')};for(const [k,,t] of DIE.fabric)col[k]=css(t);
  cx.fillStyle=css('substrate');cx.fillRect(0,0,cv.width,cv.height);
  cx.globalAlpha=showDesign?0.55:1;
  for(let y=0;y<H;y++){const r=DIE.grid[y];for(let x=0;x<W;x++){const c=r[x];if(c==='.')continue;cx.fillStyle=col[c];
    if(tall(c))cx.fillRect(x*S,(y-4)*S,S,5*S-1);else cx.fillRect(x*S,y*S,S,S-(c==='n'?0:1))}}
  cx.globalAlpha=1;cx.strokeStyle=css('muted');cx.fillStyle=css('muted');cx.lineWidth=1;cx.setLineDash([4,3]);
  cx.font='600 13px '+css('mono');
  for(const [n,x0,y0,x1,y1] of DIE.regions){cx.strokeRect(x0*S+.5,y0*S+.5,(x1-x0+1)*S-1,(y1-y0+1)*S-1);cx.fillText(n,x0*S+6,y0*S+16)}
  cx.setLineDash([]);
  if(!showDesign)return;
  for(const [k,gs] of byTile){const [x,y]=k.split(',').map(Number);let best=null,bn=-1,tot=0;
    for(const g in gs){tot+=gs[g];if(gs[g]>bn){bn=gs[g];best=+g}}
    if(focus!==null&&!gs[focus])continue;
    cx.fillStyle=css(gtok(focus===null?best:focus));cx.globalAlpha=Math.min(1,.45+tot/30);
    const t=tall(DIE.grid[y][x]);cx.fillRect(x*S-1,(t?y-4:y)*S-1,S+2,(t?5*S:S)+1)}
  cx.globalAlpha=1;cx.fillStyle=css('fg');
  for(const [x,y] of DIE.pins){cx.beginPath();cx.arc(x*S+S/2,y*S+S/2,4,0,7);cx.fill()}
}
const label=c=>c==='n'?'Interconnect switch':(DIE.fabric.find(f=>f[0]===c)||[,'Routing / spacer'])[1];
cv.addEventListener('mousemove',e=>{
  const r=cv.getBoundingClientRect(),k=cv.width/r.width,x=Math.floor((e.clientX-r.left)*k/S);let y=Math.floor((e.clientY-r.top)*k/S);
  if(x<0||y<0||x>=W||y>=H){tip.style.display='none';return}
  let c=DIE.grid[y][x];
  if(c==='.')for(let d=1;d<5&&y+d<H;d++){if(tall(DIE.grid[y+d][x])){y+=d;c=DIE.grid[y][x];break}}
  const reg=DIE.regions.find(([,x0,y0,x1,y1])=>x>=x0&&x<=x1&&y>=y0&&y<=y1);
  let t=`X${x} Y${y}${reg?'  '+reg[0]:''}\n${label(c)}`;
  const gs=byTile.get(x+','+y);
  if(gs&&showDesign)for(const g in gs)t+=`\n${DIE.groups[g]}: ${gs[g]} cell${gs[g]>1?'s':''}`;
  for(const p of DIE.pins)if(p[0]===x&&p[1]===y)t+=`\nport ${p[2]} (pin ${p[3]})`;
  tip.textContent=t;tip.style.display='block';
  const st=cv.parentElement.getBoundingClientRect();let lx=e.clientX-st.left+14;
  if(lx>st.width-200)lx=Math.max(0,e.clientX-st.left-tip.offsetWidth-14);
  tip.style.left=lx+'px';tip.style.top=(e.clientY-st.top+14)+'px'});
cv.addEventListener('mouseleave',()=>{tip.style.display='none'});
const gb=document.getElementById('groups'),bf=document.getElementById('b-fabric'),bd=document.getElementById('b-design');
function setDesign(v){showDesign=v;bf.setAttribute('aria-pressed',!v);bd.setAttribute('aria-pressed',v);gb.hidden=!v;draw()}
bf.onclick=()=>setDesign(false);bd.onclick=()=>setDesign(true);
DIE.groups.forEach((name,g)=>{const b=document.createElement('button');b.type='button';b.setAttribute('aria-pressed','false');
  const sw=document.createElement('span');sw.className='sw';sw.style.background=`var(--${gtok(g)})`;b.append(sw,name);
  b.onclick=()=>{focus=focus===g?null:g;for(const o of gb.children)o.setAttribute('aria-pressed','false');if(focus!==null)b.setAttribute('aria-pressed','true');draw()};
  gb.appendChild(b)});
const el=(tag,cls,text)=>{const e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e};
const swatch=tok=>{const td=el('td'),s=el('span','sw');s.style.background=`var(--${tok})`;td.append(s);return td};
const head=(t,cols)=>{const tr=el('tr');for(const c of cols)tr.append(el('th',null,c));t.append(tr)};
const fab=document.getElementById('fab');head(fab,['','Tile','Count']);
for(const [,l,tok,n] of DIE.fabric){if(!n)continue;const tr=el('tr');tr.append(swatch(tok),el('td',null,l),el('td','n',n.toLocaleString()));fab.append(tr)}
{const tr=el('tr');tr.append(el('td'),el('td',null,'Slices (4 LUT6, 8 FF, 1 CARRY4 each)'),el('td','n',DIE.slices.toLocaleString()));fab.append(tr)}
const use=document.getElementById('use'),cols=['LUT','FF','CARRY4','BRAM','LUTRAM'];head(use,['','Group',...cols,'Other']);
DIE.groups.forEach((name,g)=>{const s=DIE.summary[g],tr=el('tr');tr.append(swatch(gtok(g)),el('td','g',name));
  let other=0;for(const k in s)if(!cols.includes(k))other+=s[k];
  for(const c of cols)tr.append(el('td','n',s[c]||'–'));tr.append(el('td','n',other||'–'));use.append(tr)});
document.getElementById('foot').textContent=`${DIE.placed.toLocaleString()} placed cells · grouped by ${DIE.depth} hierarchy level${DIE.depth>1?'s':''} · ${DIE.version}`;
draw();
matchMedia('(prefers-color-scheme: dark)').addEventListener('change',draw);
new MutationObserver(draw).observe(document.documentElement,{attributes:true,attributeFilter:['data-theme']});
})();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    sys.exit(main())
