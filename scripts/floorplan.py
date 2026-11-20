#!/usr/bin/env python3
"""Floorplan page for the routed bitlogic Ethernet design.

Reads the routed checkpoint written by the bitstream build
(bitlogic_ethernet/build/routed.dcp), asks Vivado for the device grid and
the placement (bitlogic_ethernet/scripts/dump_floorplan.tcl), and writes one
self-contained HTML page with:

  * a block diagram of the Arty A7-100T board (not to scale): the chips and
    connectors around the FPGA and the I/O bank each one is wired to;
  * the XC7A100T floorplan: every site-bearing tile, the cells of the design
    colored by module, clock regions, I/O banks and the pins the design uses.

    python3 scripts/floorplan.py [--xilinx-root DIR] [--out PATH]

The Vivado dump is cached next to the checkpoint and redone when the
checkpoint is newer.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
FLOW = REPO / "bitlogic_ethernet"
DCP = FLOW / "build" / "routed.dcp"
DUMP = FLOW / "build" / "floorplan.tsv"
TCL = FLOW / "scripts" / "dump_floorplan.tcl"

# Module groups, in legend order: (key, label, color, description).
MODULES = [
    ("rx", "rx", "#4e79a7", "MII receive, CRC check, writes the RX ring (src/rx.rs)"),
    ("ring", "RX ring", "#a0cbe8", "rx_ram/rx_meta and the feed copy feed_rx_ram/feed_rx_meta"),
    ("feed", "feed", "#f28e2b", "IEX-TP parse and filter, PLU FIFO (src/feed.rs, plu_*)"),
    ("book", "book", "#e15759", "order book: hash, probe, list walk (src/book.rs)"),
    ("tables", "book RAM", "#ff9d9a", "sym_key/sym_meta, node_price/size/next, free_list"),
    ("bbo", "BBO queue", "#b07aa1", "bbo_ram: BBO records waiting for a frame"),
    ("proto", "proto", "#59a14f", "ARP, ICMP, beacons, BBO frame builder, template ROM (src/proto.rs)"),
    ("txbuf", "TX buffer", "#8cd17d", "tx_ram/tx_meta: frames waiting for tx"),
    ("tx", "tx", "#edc948", "MII transmit: preamble, CRC, gap (src/tx.rs)"),
    ("core", "core glue", "#9c755f", "eth_core registers outside the units"),
    ("top", "board top", "#bab0ac", "arty_top.v: PLL, global clock buffers, resets, TX clock FIFO, LED status unit"),
    ("io", "I/O pads", "#76b7b2", "IBUF/OBUF pad buffers, the PHY clock ODDR and the MII TX output registers (in the I/O tiles)"),
]
MODULE_INDEX = {m[0]: i for i, m in enumerate(MODULES)}

UNIT_OF = {
    "rx_i": "rx", "rx_ram_i": "ring", "rx_meta_i": "ring", "feed_rx_ram_i": "ring",
    "feed_rx_meta_i": "ring", "feed_i": "feed", "plu_symbol_i": "feed", "plu_price_i": "feed",
    "plu_misc_i": "feed", "book_i": "book", "sym_key_i": "tables", "sym_meta_i": "tables",
    "node_price_i": "tables", "node_size_i": "tables", "node_next_i": "tables",
    "free_list_i": "tables", "bbo_ram_i": "bbo", "proto_i": "proto", "template_i": "proto",
    "tx_ram_i": "txbuf", "tx_meta_i": "txbuf", "tx_i": "tx",
}

# Board parts and the FPGA banks they connect to. Used: wired by this design.
# Pin assignments for the parts this design does not use follow Digilent's
# Arty A7 master XDC and reference manual.
BOARD = [
    dict(id="eth", label="Ethernet PHY", part="TI DP83848J, 10/100, MII", banks=[15], used_banks=[15], used=True,
         note="eth_rx_clk, eth_rx_dv, eth_rxd[3:0], eth_rxerr, eth_tx_clk, eth_tx_en, eth_txd[3:0], eth_ref_clk (25 MHz from the FPGA), eth_rstn"),
    dict(id="rj45", label="RJ45 jack", part="magnetics, 100BASE-TX", banks=[], used_banks=[], used=True, note="cable to eno1"),
    dict(id="osc", label="100 MHz oscillator", part="CLK100MHZ, pin E3", banks=[35], used_banks=[35], used=True,
         note="system clock; the PLL makes the PHY's 25 MHz reference from it"),
    dict(id="leds", label="LEDs LD4-LD7", part="H5, J5, T9, T10", banks=[35, 14], used_banks=[35, 14], used=True,
         note="frame counter and RX/TX activity (status_unit)"),
    dict(id="usb", label="USB JTAG + UART", part="FTDI FT2232HQ, micro-USB", banks=[0, 16], used_banks=[0], used=True,
         note="JTAG (dedicated config pins) programs the FPGA; UART pins A9/D10 unused"),
    dict(id="ddr", label="DDR3L SDRAM", part="256 MB, 16-bit", banks=[34], used_banks=[], used=False,
         note="bank 34; needs a memory controller (MIG), not used"),
    dict(id="flash", label="Quad-SPI flash", part="16 MB, config + data", banks=[14], used_banks=[], used=False,
         note="holds a bitstream for power-on boot; this design is loaded over JTAG"),
    dict(id="sw", label="Switches, buttons, RGB", part="SW0-3, BTN0-3, LD0-3", banks=[16, 35], used_banks=[], used=False, note="not used"),
    dict(id="pmod", label="Pmod JA-JD", part="4 x 12-pin headers", banks=[15, 14, 35], used_banks=[], used=False,
         note="JA, JB: bank 15; JC: bank 14; JD: bank 35; not used"),
    dict(id="shield", label="Shield header", part="Arduino/chipKIT IO0-IO41", banks=[14, 16, 35], used_banks=[], used=False, note="not used"),
]


def module_of(cell, site):
    if site.startswith(("IOB_", "OLOGIC_", "ILOGIC_")):
        return "io"
    parts = cell.split("/")
    if parts[0] == "eth_core_i":
        if len(parts) > 2 and parts[1] in UNIT_OF:
            return UNIT_OF[parts[1]]
        return "core"
    return "top"


def run_dump(xilinx_root):
    vivado = Path(xilinx_root) / "Vivado" / "bin" / "vivado"
    if not vivado.exists():
        sys.exit(f"Vivado not found at {vivado}; pass --xilinx-root")
    log_dir = DUMP.parent / "floorplan_vivado"
    log_dir.mkdir(exist_ok=True)
    cmd = [str(vivado), "-mode", "batch", "-nojournal", "-log", str(log_dir / "vivado.log"),
           "-source", str(TCL), "-tclargs", str(DCP), str(DUMP)]
    print("reading the routed design with Vivado (about a minute)...", flush=True)
    subprocess.run(cmd, check=True, cwd=log_dir, stdout=subprocess.DEVNULL)


def category(tile_type):
    t = tile_type
    if t.startswith("INT"):
        return "int"
    if t.startswith("GTP"):
        return "gt"
    if t.startswith("CLBLM"):
        return "clbm"
    if t.startswith("CLBLL"):
        return "clbl"
    if t.startswith("BRAM"):
        return "bram"
    if t.startswith("DSP"):
        return "dsp"
    if "IOB" in t:
        return "iob"
    if t.startswith(("LIOI", "RIOI")):
        return "ioi"
    if t.startswith("CMT") or "PLL" in t or "MMCM" in t:
        return "cmt"
    if t.startswith(("CLK", "HCLK")):
        return "clk"
    return "other"


def build(dump_path):
    tiles, sites, regions, pins, ports, cells = {}, {}, [], {}, [], []
    part = grid = None
    for line in Path(dump_path).read_text().splitlines():
        f = line.split("\t")
        kind = f[0]
        if kind == "tile":
            tiles[f[1]] = dict(type=f[2], row=int(f[3]), col=int(f[4]))
        elif kind == "site":
            sites[f[1]] = dict(type=f[2], tile=f[3], region=f[4])
        elif kind == "region":
            regions.append(dict(name=f[1], r0=int(f[2]), c0=int(f[3]), r1=int(f[4]), c1=int(f[5])))
        elif kind == "pin":
            pins[f[1]] = dict(bank=f[2], site=f[3])
        elif kind == "port":
            ports.append(dict(name=f[1], pin=f[2], dir=f[3]))
        elif kind == "cell":
            cells.append(dict(name=f[1], site=f[2], bel=f[3], ref=f[4]))
        elif kind == "part":
            part = f[1]
        elif kind == "grid":
            grid = dict(rows=int(f[2]), cols=int(f[3]))

    # Tile heights. A tile's ROW is the bottom row of its span; it reaches up
    # to the previous site-bearing tile in its column, capped by type (BRAM
    # and DSP tiles span five interconnect rows, I/O tiles two).
    cap = {"bram": 5, "dsp": 5, "iob": 2, "ioi": 2, "cmt": 13, "clk": 13, "gt": 16, "other": 13}
    by_col = defaultdict(list)
    for name, t in tiles.items():
        t["cat"] = category(t["type"])
        if t["cat"] != "int":
            by_col[t["col"]].append(name)
    for col, names in by_col.items():
        names.sort(key=lambda n: tiles[n]["row"])
        prev = -1
        for n in names:
            t = tiles[n]
            t["h"] = max(1, min(t["row"] - prev, cap.get(t["cat"], 1)))
            t["top"] = t["row"] - t["h"] + 1
            prev = t["row"]
    tiles = {n: t for n, t in tiles.items() if t["cat"] != "int"}

    # Cells per site, by module.
    site_cells = defaultdict(list)
    stats = defaultdict(Counter)
    lut_seen = set()
    for c in cells:
        mod = module_of(c["name"], c["site"])
        c["mod"] = mod
        site_cells[c["site"]].append(c)
        ref = c["ref"]
        s = stats[mod]
        if ref.startswith("LUT"):
            # Two LUT cells can share one LUT6 (A6LUT + A5LUT): count sites.
            key = (c["site"], c["bel"][-5:-4] if c["bel"].endswith(("6LUT", "5LUT")) else c["bel"])
            if key not in lut_seen:
                lut_seen.add(key)
                s["LUT"] += 1
        elif ref.startswith("FD"):
            s["FF"] += 1
        elif ref == "RAMB36E1":
            s["BRAM"] += 2  # in RAMB18 halves, shown as tiles / 2
        elif ref == "RAMB18E1":
            s["BRAM"] += 1
        elif ref.startswith(("RAM32", "RAM64", "RAMD", "RAMS", "SRL")):
            key = (c["site"], c["bel"].split(".")[-1][:1])
            if key not in lut_seen:
                lut_seen.add(key)
                s["LUTRAM"] += 1
        elif ref == "CARRY4":
            s["CARRY4"] += 1
        elif ref.startswith("MUXF"):
            s["MUXF"] += 1

    used = []
    for site, cs in site_cells.items():
        info = sites.get(site)
        if not info or info["tile"] not in tiles:
            continue
        t = tiles[info["tile"]]
        mods = Counter(c["mod"] for c in cs)
        main = mods.most_common(1)[0][0]
        # Two slices per CLB tile: draw each in its own half (SLICE_X even/odd).
        half = -1
        m = re.match(r"SLICE_X(\d+)Y", site)
        if m:
            half = int(m.group(1)) % 2
        ram = re.match(r"RAMB(36|18)_X\d+Y(\d+)", site)
        if ram and ram.group(1) == "18":
            half = 2 + int(ram.group(2)) % 2  # lower/upper RAMB18 of the tile
        iob = re.match(r"(IOB|ILOGIC|OLOGIC)_X\d+Y(\d+)", site)
        if iob and t["h"] == 2:
            half = 2 + int(iob.group(2)) % 2
        refs = Counter(c["ref"] for c in cs)
        names = sorted({c["name"].rsplit("/", 1)[0] if "/" in c["name"] else c["name"] for c in cs})
        used.append([t["col"], t["top"], t["h"], MODULE_INDEX[main], half, site, info["type"],
                     ", ".join(f"{n} {r}" for r, n in refs.most_common()),
                     "; ".join(f"{MODULES[MODULE_INDEX[k]][1]} {v}" for k, v in mods.most_common()),
                     names[:4] + (["…"] if len(names) > 4 else [])])

    # I/O banks: extent of each bank's pad tiles.
    banks = defaultdict(list)
    for pin, p in pins.items():
        if p["site"] in sites and sites[p["site"]]["tile"] in tiles:
            t = tiles[sites[p["site"]]["tile"]]
            if p["bank"].isdigit() and t["cat"] == "iob":
                banks[p["bank"]].append(t)
    bank_list = []
    for b, ts in sorted(banks.items()):
        bank_list.append(dict(bank=int(b), r0=min(t["top"] for t in ts), r1=max(t["row"] + 1 for t in ts),
                              c0=min(t["col"] for t in ts), c1=max(t["col"] for t in ts) + 1, pins=len(ts)))

    port_list = []
    for p in ports:
        pin = pins.get(p["pin"])
        if not pin or pin["site"] not in sites:
            continue
        t = tiles.get(sites[pin["site"]]["tile"])
        if not t:
            continue
        port_list.append(dict(name=p["name"], pin=p["pin"], bank=pin["bank"], dir=p["dir"],
                              site=pin["site"], col=t["col"], row=t["top"], h=t["h"]))

    tile_list = [[t["col"], t["top"], t["h"], t["cat"]] for t in tiles.values()]
    totals = Counter()
    for s in stats.values():
        totals.update(s)
    cap_counts = Counter()
    for s in sites.values():
        st = s["type"]
        if st in ("SLICEL", "SLICEM"):
            cap_counts["slices"] += 1

    return dict(
        part=part, grid=grid, tiles=tile_list, used=used, regions=regions, banks=bank_list,
        ports=sorted(port_list, key=lambda p: p["name"]),
        modules=[dict(key=k, label=l, color=c, desc=d, stats=dict(stats.get(k, {}))) for k, l, c, d in MODULES],
        totals=dict(totals), capacity=dict(LUT=cap_counts["slices"] * 4, FF=cap_counts["slices"] * 8,
                                           BRAM=2 * sum(1 for t in tiles.values() if t["cat"] == "bram")),
        board=BOARD,
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xilinx-root", default=os.environ.get("XILINX_ROOT", "/mnt/extra_pool/Xilinx/2025.2"))
    ap.add_argument("--out", type=Path, default=FLOW / "build" / "floorplan.html")
    args = ap.parse_args()
    if not DCP.exists():
        sys.exit(f"{DCP} not found; run python3 build.py build (it writes the routed checkpoint)")
    if not DUMP.exists() or DUMP.stat().st_mtime < DCP.stat().st_mtime:
        run_dump(args.xilinx_root)
    data = build(DUMP)
    page = (Path(__file__).with_name("floorplan_template.html")).read_text()
    blob = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    args.out.write_text(page.replace("/*DATA*/null", blob, 1))
    t = data["totals"]
    print(f"wrote {args.out}: {data['part']}, {len(data['used'])} used sites, "
          f"{t.get('LUT', 0)} LUTs, {t.get('FF', 0)} FFs, {t.get('BRAM', 0) / 2:g} BRAM tiles")


if __name__ == "__main__":
    main()
