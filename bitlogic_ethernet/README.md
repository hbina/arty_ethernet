# bitlogic Ethernet endpoint

A reimplementation of the `hls_ethernet/` Layer-2 endpoint using
bitlogic (sibling checkout `../../bitlogic`). It has the same identity: MAC
`02:00:00:00:00:01` and IP `192.168.1.100`. It replies to ARP requests, sends a
gratuitous ARP every 5 s, and sends a diagnostics beacon (EtherType `0x88B5`)
every 1 s. Beyond the HLS design, it checks the FCS of every received frame
and answers ICMP echo requests, so `ping 192.168.1.100` should work (verified
in simulation; not yet on the board). It uses the same pins and XDC (`constraints/arty_a7.xdc`).

```sh
python3 build.py build --design bitlogic        # generate RTL, then Vivado → bitlogic_ethernet/build/bitlogic_ethernet.bit
python3 build.py program --bitfile bitlogic_ethernet/build/bitlogic_ethernet.bit
cargo test --release --locked --manifest-path bitlogic_ethernet/Cargo.toml
```

## How it is split

bitlogic only expresses pure, untimed functions `state → next state`. All
behavior lives in four `#[logic]` unit functions and the `#[logic]` helpers they call (`src/ops.rs`). Everything with time, storage or
physical meaning is Verilog.

| Piece | Kind | File |
|---|---|---|
| MII RX assembly, preamble/SFD, FCS check and strip, RX ring producer | bitlogic | `src/rx.rs` |
| RX consumer, ARP and IPv4/ICMP parse and checksums, timers, TX frame preparation (ROM templates and echo-reply copy), counters | bitlogic | `src/proto.rs` |
| TX ring consumer, preamble/SFD, padding, CRC-32 FCS, IFG | bitlogic | `src/tx.rs` |
| LED activity/frame-count status (100 MHz domain) | bitlogic | `src/status.rs` |
| Shared helpers: hex digit, byte/nibble select, field shift-in, counters, CRC-32 byte, Internet checksum arithmetic | bitlogic (inlined at each call) | `src/ops.rs` |
| Beacon and ARP frame templates (512 × 9 ROM) | generated from Rust | `src/rom.rs` |
| Registered unit wrappers (state register + step) | generated from Rust | `src/hdl.rs` |
| RAMs, MII boundary registers, unit wiring | handwritten Verilog | `rtl/eth_core.v`, `rtl/sdp_ram.v` |
| PLL, PHY reference clock, resets, CDC | handwritten Verilog | `rtl/arty_top.v` |

```mermaid
flowchart LR
    subgraph eth["eth_rx_clk domain (eth_core.v)"]
        direction LR
        miiin["MII RX regs"] --> rx["rx_unit"]
        rx -- "write" --> rxram[("RX RAM 8×2 KiB<br/>+ meta")]
        rxram -- "read (1 cycle)" --> proto["proto_unit"]
        rom[("template ROM")] -- "read (1 cycle)" --> proto
        proto -- "write" --> txram[("TX RAM 4×2 KiB<br/>+ meta")]
        txram -- "read (1 cycle)" --> tx["tx_unit"]
        tx --> miiout["MII TX regs"]
        proto -. "rx_rd_ptr" .-> rx
        rx -. "rx_wr_ptr, drops" .-> proto
        tx -. "tx_rd_ptr" .-> proto
        proto -. "tx_wr_ptr" .-> tx
    end
    subgraph sys["CLK100MHZ domain (arty_top.v)"]
        status["status_unit → LEDs"]
    end
    tx -- "frame toggle (3-flop sync)" --> status
    miiin -- "activity (2-flop sync)" --> status
```

Design rules that make the split work:

- **Each unit owns its state register.** The units are generated as
  `<unit>_logic` (pure bitlogic) plus `<unit>` (register + `s_<field>` outputs).
  Units only read each other's *registered* state (dotted lines). No
  combinational path crosses units.
- **Rings use pointers, not shared valid bits.** The producer owns `wr_ptr`,
  the consumer owns `rd_ptr`, and each pointer has one wrap bit. Each register
  has exactly one writer.
- **Memory latency is part of the state.** RAM and ROM reads take one cycle.
  Each unit drives its read address from its *next* state, so the read data
  during a cycle always belongs to the current state. Reads that might collide
  with a write are always discarded.
- **Helpers are `#[logic]` functions.** The units call `src/ops.rs` helpers
  (`crc32_byte`, `hex_digit`, `shift_in48`, …). bitlogic inlines each call, so
  helpers add no ports or registers; each call is its own copy of the logic.
- **Frame templates live in a ROM by choice.** The 143-byte beacon and 42-byte
  ARP frames are constant except for a few fields. A 512 × 9 ROM (half a BRAM)
  plus a substitution step is cheaper than building each byte from the index in
  logic, so the ROM stays even though helpers are now available.
- **Config is an input.** MAC, IP and the timer intervals enter `proto_step` as
  inputs, tied to `eth_core` parameters. Tests use short intervals.

## Behavior relative to the HLS design

ARP and beacon behavior match at the frame level. Cycle timing differs, because
HLS scheduled its own pipeline and this design adds a register on each MII
boundary and uses explicit memory reads.

- RX: frames with `rxerr`, or shorter than header + FCS, are not queued. Unlike
  HLS, the FCS is checked: the CRC-32 register runs over the whole frame
  including the FCS and must end at the residue `0xDEBB20E3`. Failing frames
  are not queued and are counted in `FCS`. Bodies over 1514 bytes are marked
  truncated and counted as protocol drops.
- ICMP echo (new): a request is answered when it is unicast to our MAC and IP,
  IPv4 with no options (IHL 5) and not fragmented, protocol 1, type 8 code 0,
  the datagram fits in the frame, and both the IPv4 header checksum and the
  ICMP checksum verify. The reply copies the request from the RX slot to a TX
  slot, swapping addresses, setting TTL 64 and type 0, and updating both
  checksums incrementally (RFC 1624). Everything else (TOS, ID, flags,
  identifier, sequence, data) is copied. Ethernet padding is not echoed.
  Requests up to 1472 data bytes (a 1500-byte datagram) are answered.
- Protocol: `RX`, `RXQ`, `TXD`, `ARP` and `UP` have the HLS semantics. `RXP`
  counts frames that are neither a valid ARP request nor a valid echo request.
  `ICMP` counts valid echo requests (a full TX ring drops the reply and
  increments `TXD`, as for ARP). The handler now reads every received byte, so
  timer ticks stay pending until IDLE serves them instead of being lost while
  it is busy. A gratuitous ARP goes before a beacon that is due at the same
  time; the HLS design drops that beacon.
- TX: frames are padded to 60 bytes, the FCS is sent LSB first, and the IFG is
  24 nibble cycles. The beacon text matches the HLS beacon up to `UP=`, then
  appends ` FCS=xxxxxxxx ICMP=xxxxxxxx` (170-byte frame body).

Changes from the HLS top level:

- The Ethernet domain gets a proper reset synchronizer: asynchronous assert on
  PLL unlock, synchronous release on `eth_rx_clk`.
- The activity signal is registered in its source domain before it crosses.

## Verification

`cargo test --release --locked` runs:

| Test | Evidence |
|---|---|
| `tests/steps.rs` | Native Rust vs bitlogic IR on 20,000 random inputs per function, including unreachable states; LED sequence; `ops.rs` helpers vs independent definitions (hex, byte/nibble select, CRC-32 vs the MSB-first reference, ones'-complement addition, RFC 1624 update vs recomputing, the RX CRC residue) |
| `tests/endpoint.rs` | Cycle model of `eth_core.v`: ARP reply bytes, ignored frames, beacon counters/text, gratuitous ARP, gratuitous ARP then beacon on coinciding ticks, IFG, maximum/oversized frames, TX ring overflow; the IR backend matches native cycle for cycle on ARP, ICMP and bad-FCS traffic |
| `tests/icmp.rs` | Byte-exact echo replies for 0, 1, 17, 56 and 1472 data bytes with nonzero sender padding; 13 invalid variants (wrong IP/MAC, broadcast, bad IPv4 or ICMP checksum, fragments, options, other protocol, echo reply, nonzero code, bad lengths) are counted, not answered; bad-FCS frames are dropped and counted; a beacon tick during a long echo is not lost; 40 random frames (valid, invalid, bad FCS, ARP) match the oracle |
| `tests/hdl.rs` | Generated + handwritten Verilog in Vivado xsim matches the Rust cycle model on all 24,000 cycles of a mixed scenario including echo requests and a bad FCS |
| `tests/top.rs` | Board-level xsim of `arty_top` with Xilinx PLL/BUFG/ODDR models and an asynchronous PHY clock (see below) |

Expected frames, the CRC (MSB-first form), MII encoding/decoding, the RFC 1071
Internet checksum and ICMP request/reply builders live in `tests/common`,
written independently of the design. Expected echo replies recompute both
checksums from scratch rather than updating them. Disabling pending ticks, the
ICMP checksum update, FCS enforcement or ICMP checksum verification each makes
at least one test fail. The xsim tests need Vivado
(`XILINX_ROOT`, default `~/Xilinx/2025.2`). Set `BITLOGIC_ETHERNET_SKIP_HDL=1`
to skip them explicitly.

### Board-level simulation

`sim/tb_arty_top.v` simulates the whole FPGA top: Xilinx `unisims_ver` models of
the PLL, BUFG and ODDR, the reset synchronizer, both clock-domain crossings and
the LED unit. The PHY RX clock starts only after `eth_rstn` releases, with an
arbitrary phase and a 100 ppm offset from the 100 MHz board clock. The timers
are shortened through `arty_top` parameters (beacon 3,000 cycles, gratuitous
ARP 7,000, LED holdoff 200). The parameter defaults are the board values.

`tests/top.rs` runs 10,000 PHY cycles (about 0.4 ms of simulated time) and checks:

- the PLL locks (about 0.57 µs), `eth_rstn` releases, and `eth_ref_clk` is 40.000 ns;
- the TX pins match the Rust cycle model on every cycle after reset;
- the frames are an ARP reply, an ICMP echo reply, two beacons, a gratuitous ARP
  and a third beacon, with correct counters and FCS; a request with a corrupted
  FCS is counted and not answered;
- `led[3:1]` counts each of the 6 frames once through the toggle synchronizer,
  and `led[0]` lights on activity and clears after the holdoff.

Breaking the toggle synchronizer's edge detect makes this test fail.

To inspect waveforms after `cargo test --release --test top`:

```sh
cd bitlogic_ethernet/target/tmp/xsim_arty_top
~/Xilinx/2025.2/Vivado/bin/xsim tb_arty_top_sim -gui
```

## Implementation results

Vivado 2025.2, `xc7a100ticsg324-1L`, measured 2026-11-18:

| Metric | bitlogic (ARP + beacon + FCS check + ICMP echo) | HLS design (ARP + beacon) |
|---|---|---|
| Slice LUTs | 1,304 | 2,228 |
| Slice registers | 1,510 | 1,334 |
| Block RAM tiles | 6.5 | — |
| Setup WNS / hold WHS | +5.047 ns / +0.116 ns, 0 failing endpoints | — |
| `eth_rx_clk` (40 ns) setup slack | +25.679 ns | — |
| `report_cdc` | 2 endpoints, both Safe (ASYNC_REG synchronizers) | — |

Before FCS checking and ICMP echo the bitlogic design used 889 LUTs and 1,050
registers. The overall WNS is in the 100 MHz LED domain.

No critical warnings were reported. These are post-route static results, not
board validation.

## Limits

- Simulation does not model PHY timing, MII pad delays or a real link.
  Board-level validation still uses the existing `tests/hw` suite. Run
  `python3 build.py test --hardware --bitfile bitlogic_ethernet/build/bitlogic_ethernet.bit`
  after programming.
- As in the HLS design, TX is clocked by `eth_rx_clk`, not the PHY's
  `eth_tx_clk`, and the XDC has no MII input/output delay constraints.
- RX ring overflow is implemented but not exercised. The protocol unit
  processes frames faster than they arrive (one cycle per byte to parse, plus
  one per byte to copy an echo request, against two cycles per byte on the
  wire), so the ring cannot fill in these tests.
- ICMP echo is the only IPv4 service. There is no IPv4 option handling,
  fragment reassembly, broadcast ping or ICMP error generation. The
  `tests/hw/test_icmp.py` board test exists but has not been run.
- The timer intervals assume a 100 Mb/s link. At 10 Mb/s the MII clock is
  2.5 MHz, so beacons slow by 10× (same as HLS).
