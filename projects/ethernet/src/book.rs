//! Order book: apply IEX DEEP Price Level Updates (PLUs) per symbol and side,
//! and emit the best bid and offer (BBO) of a symbol when a PLU ends an atomic
//! event (Event Flags bit 0 set).
//!
//! Storage, all in block RAM with one read and one write port each:
//! - Symbol table, `SYMBOL_SLOTS` entries in two parallel RAMs: `sym_key`
//!   (the 8-byte symbol) and `sym_meta` (`{valid, bid_head, ask_head}`).
//!   Open addressing: [`symbol_hash`] picks the first slot, then linear
//!   probing, at most `MAX_PROBES` slots. Symbols are never removed.
//! - A pool of price-level nodes in three parallel RAMs (`node_price`,
//!   `node_size`, `node_next`), linked into one sorted list per symbol and
//!   side: bids by descending price, offers by ascending price, so each
//!   list's head is the best level. Node 0 is the null link and is never
//!   allocated. Never-used nodes are taken in order (`used + 1`); deleted
//!   nodes are pushed onto a free stack (`free_list` RAM) and reused first.
//!
//! A PLU of size zero deletes its price level; any other size sets it
//! (inserting the level if absent). Deleting an absent level changes
//! nothing, as when the feed starts after the level was added.
//!
//! Phases ([`BookPhase`]; data only one phase needs is carried by its variant):
//! - `Clear` (0, the reset phase): invalidate every symbol slot, one per cycle.
//! - `Idle`: wait for a PLU and for room in the BBO FIFO.
//! - `Load`: latch the PLU (FIFO read data) and hash its symbol.
//! - `Probe { probes }`: compare one symbol slot per cycle; a vacant slot is
//!   claimed.
//! - `Walk(Cursor)`: follow the side's list one node per cycle until the first
//!   node that is not better than the PLU price, then update, delete or insert
//!   there. Inserting after an existing node needs `Link { after, node }` to
//!   rewrite that node's `next`.
//! - `Finish`: write the symbol's list heads back.
//! - `BidRead`, `AskRead { bid }`, `Emit { bid, ask, word }`: on an
//!   event-ending PLU, read both heads and write the 32-byte BBO record into
//!   the BBO FIFO as four 64-bit words: symbol, bid price, offer price,
//!   `{offer size, bid size}`. An empty side reads as price 0 and size 0.
//!
//! Every RAM read address is derived from the next state, so the read data in
//! a cycle belongs to the current state. A RAM never returns data written on
//! the same edge as the read, so every read is issued at least one cycle after
//! the last write it depends on.
//!
//! Failures (symbol table full, a list longer than `MAX_HOPS`, node pool
//! exhausted) drop the PLU and count in `errors`.
use crate::ops::bump32;
use bitlogic::{Bits, LogicValue, bits, logic};

pub const SYMBOL_ADDR_BITS: u32 = 11;
pub const SYMBOL_SLOTS: usize = 1 << SYMBOL_ADDR_BITS;
pub const NODE_ADDR_BITS: u32 = 13;
/// Usable nodes: node 0 is the null link.
pub const NODES: usize = (1 << NODE_ADDR_BITS) - 1;
pub const MAX_PROBES: u64 = 16;
pub const MAX_HOPS: u64 = 64;
/// BBO FIFO depth in records; pointers carry one wrap bit.
pub const BBO_FIFO_DEPTH: usize = 256;

/// A position in one side's price list.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Cursor {
    /// The node read this cycle (0: the end of the list).
    pub cur: Bits<13>,
    /// The node before it (0: `cur` is the head).
    pub prev: Bits<13>,
    pub hops: Bits<7>,
}

/// The book's phase, with the data that only that phase uses. `Clear` comes
/// first: the register resets to all zeros.
#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub enum BookPhase {
    Clear,
    Idle,
    Load,
    /// Symbol slots compared so far, minus one.
    Probe {
        probes: Bits<5>,
    },
    Walk(Cursor),
    /// Point `after.next` at the newly inserted `node`.
    Link {
        after: Bits<13>,
        node: Bits<13>,
    },
    Finish,
    BidRead,
    AskRead {
        bid: Level,
    },
    /// Writing BBO word `word` of the record.
    Emit {
        bid: Level,
        ask: Level,
        word: Bits<2>,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Plu {
    pub sell: bool,
    pub event: bool,
    pub symbol: Bits<64>,
    pub size: Bits<32>,
    pub price: Bits<64>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct Level {
    pub price: Bits<64>,
    pub size: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct BookCounters {
    /// PLUs taken from the FIFO.
    pub updates: Bits<32>,
    /// BBO records written.
    pub bbos: Bits<32>,
    /// Symbols added to the table.
    pub symbols: Bits<32>,
    /// Price levels currently held.
    pub levels: Bits<32>,
    /// PLUs dropped: table full, list too long or pool exhausted.
    pub errors: Bits<32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct BookState {
    pub phase: BookPhase,
    pub plu: Plu,
    /// Consumer side of the PLU FIFO.
    pub plu_rd_ptr: Bits<7>,
    /// Symbol slot being probed, then written back in `Finish` (also the
    /// clear index).
    pub slot: Bits<11>,
    /// List heads of the current symbol.
    pub bid_head: Bits<13>,
    pub ask_head: Bits<13>,
    /// Never-used nodes taken so far; the next is `used + 1`.
    pub used: Bits<13>,
    /// Entries on the free stack.
    pub free_sp: Bits<14>,
    /// Producer side of the BBO FIFO.
    pub bbo_wr_ptr: Bits<9>,
    pub counters: BookCounters,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct BookIn {
    pub state: BookState,
    pub plu_wr_ptr: Bits<7>,
    /// PLU FIFO entry at `plu_rd_ptr`.
    pub plu_symbol: Bits<64>,
    pub plu_price: Bits<64>,
    pub plu_misc: Bits<34>,
    /// Symbol slot at `slot`.
    pub sym_key: Bits<64>,
    pub sym_meta: Bits<27>,
    /// Node at the address chosen by the previous state (see `node_raddr`).
    pub node_price: Bits<64>,
    pub node_size: Bits<32>,
    pub node_next: Bits<13>,
    /// Free stack top: entry `free_sp - 1`.
    pub free_top: Bits<13>,
    /// Consumer side of the BBO FIFO (the protocol unit).
    pub bbo_rd_ptr: Bits<9>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, LogicValue)]
pub struct BookOut {
    pub state: BookState,
    pub plu_raddr: Bits<6>,
    pub sym_raddr: Bits<11>,
    pub sym_waddr: Bits<11>,
    pub sym_key_we: bool,
    pub sym_key_wdata: Bits<64>,
    pub sym_meta_we: bool,
    pub sym_meta_wdata: Bits<27>,
    pub node_raddr: Bits<13>,
    /// Price and size share an address: the inserted or updated node.
    pub node_waddr: Bits<13>,
    pub node_price_we: bool,
    pub node_price_wdata: Bits<64>,
    pub node_size_we: bool,
    pub node_size_wdata: Bits<32>,
    pub node_next_we: bool,
    pub node_next_waddr: Bits<13>,
    pub node_next_wdata: Bits<13>,
    pub free_raddr: Bits<13>,
    pub free_we: bool,
    pub free_waddr: Bits<13>,
    pub free_wdata: Bits<13>,
    pub bbo_we: bool,
    pub bbo_waddr: Bits<10>,
    pub bbo_wdata: Bits<64>,
}

/// First symbol-table slot for a symbol: XOR the symbol with shifted copies
/// of itself, then fold the 64 bits into 11. Chosen on the symbols of the
/// sample capture (at most 5 probes for 579 symbols in 2048 slots).
#[logic]
pub fn symbol_hash(symbol: Bits<64>) -> Bits<11> {
    let x = symbol ^ (symbol >> 29) ^ (symbol >> 17) ^ (symbol << 7);
    x.slice::<11>(0)
        ^ x.slice::<11>(11)
        ^ x.slice::<11>(22)
        ^ x.slice::<11>(33)
        ^ x.slice::<11>(44)
        ^ bits::<2>(0).concat::<9, 11>(x.slice::<9>(55))
}

/// `{valid, bid_head, ask_head}`.
#[logic]
fn pack_meta(bid_head: Bits<13>, ask_head: Bits<13>) -> Bits<27> {
    bits::<1>(1)
        .concat::<13, 14>(bid_head)
        .concat::<13, 27>(ask_head)
}

/// The level at a list head, or the empty level for an empty list.
#[logic]
fn head_level(head: Bits<13>, node: Level) -> Level {
    if head == bits::<13>(0) {
        Level {
            price: bits::<64>(0),
            size: bits::<32>(0),
        }
    } else {
        node
    }
}

#[logic]
pub fn book_step(input: BookIn) -> BookOut {
    let s = input.state;
    let q = s.plu;
    let n = s.counters;

    let clearing = s.phase == BookPhase::Clear;
    let idle = s.phase == BookPhase::Idle;
    let loading = s.phase == BookPhase::Load;
    let finishing = s.phase == BookPhase::Finish;
    let probing = matches!(s.phase, BookPhase::Probe { .. });
    let walking = matches!(s.phase, BookPhase::Walk(_));
    let linking = matches!(s.phase, BookPhase::Link { .. });
    let emitting = matches!(s.phase, BookPhase::Emit { .. });
    // Phase data, read as zero outside its phase.
    let probes = match s.phase {
        BookPhase::Probe { probes } => probes,
        _ => bits::<5>(0),
    };
    let walk = match s.phase {
        BookPhase::Walk(c) => c,
        _ => Cursor {
            cur: bits::<13>(0),
            prev: bits::<13>(0),
            hops: bits::<7>(0),
        },
    };
    let word = match s.phase {
        BookPhase::Emit { word, .. } => word,
        _ => bits::<2>(0),
    };

    // Idle: take a PLU only when its BBO record is sure to fit.
    let plu_avail = s.plu_rd_ptr != input.plu_wr_ptr;
    let bbo_space = s.bbo_wr_ptr - input.bbo_rd_ptr != bits::<9>(256);
    let take = idle && plu_avail && bbo_space;
    let clear_done = clearing && s.slot == bits::<11>(2047);

    // Load.
    let loaded = Plu {
        sell: input.plu_misc.bit(33),
        event: input.plu_misc.bit(32),
        symbol: input.plu_symbol,
        size: input.plu_misc.slice::<32>(0),
        price: input.plu_price,
    };

    // Probe.
    let meta_valid = input.sym_meta.bit(26);
    let meta_bid = input.sym_meta.slice::<13>(13);
    let meta_ask = input.sym_meta.slice::<13>(0);
    let hit = probing && meta_valid && input.sym_key == q.symbol;
    let vacant = probing && !meta_valid;
    let collide = probing && meta_valid && input.sym_key != q.symbol;
    let table_full = collide && probes == bits::<5>(15);
    let start_walk = hit || vacant;
    let hit_head = if q.sell { meta_ask } else { meta_bid };

    // Walk: the current node is better than the PLU price (keep going), at
    // the PLU price, or past it / the end of the list (stop here).
    let at_end = walk.cur == bits::<13>(0);
    let np = input.node_price;
    let nn = input.node_next;
    let better = if q.sell { np < q.price } else { np > q.price };
    let before = walking && !at_end && better;
    let same = walking && !at_end && np == q.price;
    let stop = walking && !before;
    let too_deep = before && walk.hops == bits::<7>(63);
    let advance = before && !too_deep;
    let zero_size = q.size == bits::<32>(0);
    let deleting = same && zero_size;
    let updating = same && !zero_size;
    let want_insert = stop && !same && !zero_size;
    let have_free = s.free_sp != bits::<14>(0);
    let fresh_left = s.used != bits::<13>(8191);
    let pool_empty = want_insert && !have_free && !fresh_left;
    let inserting = want_insert && (have_free || fresh_left);
    let alloc = if have_free {
        input.free_top
    } else {
        s.used + bits::<13>(1)
    };
    let at_head = walk.prev == bits::<13>(0);
    let set_head = (deleting || inserting) && at_head;
    let new_head = if deleting { nn } else { alloc };
    let need_link = inserting && !at_head;

    // BidRead/AskRead: the node read is the head of that side.
    let node_level = Level {
        price: np,
        size: input.node_size,
    };
    let emit_last = emitting && word == bits::<2>(3);

    let next_phase = match s.phase {
        BookPhase::Clear => {
            if clear_done {
                BookPhase::Idle
            } else {
                BookPhase::Clear
            }
        }
        BookPhase::Idle => {
            if take {
                BookPhase::Load
            } else {
                BookPhase::Idle
            }
        }
        BookPhase::Load => BookPhase::Probe {
            probes: bits::<5>(0),
        },
        BookPhase::Probe { probes } => {
            if start_walk {
                BookPhase::Walk(Cursor {
                    cur: if hit { hit_head } else { bits::<13>(0) },
                    prev: bits::<13>(0),
                    hops: bits::<7>(0),
                })
            } else if table_full {
                BookPhase::Idle
            } else {
                BookPhase::Probe {
                    probes: probes + bits::<5>(1),
                }
            }
        }
        BookPhase::Walk(c) => {
            if advance {
                BookPhase::Walk(Cursor {
                    cur: nn,
                    prev: c.cur,
                    hops: c.hops + bits::<7>(1),
                })
            } else if too_deep {
                BookPhase::Idle
            } else if need_link {
                BookPhase::Link {
                    after: c.prev,
                    node: alloc,
                }
            } else {
                BookPhase::Finish
            }
        }
        BookPhase::Link { .. } => BookPhase::Finish,
        BookPhase::Finish => {
            if q.event {
                BookPhase::BidRead
            } else {
                BookPhase::Idle
            }
        }
        BookPhase::BidRead => BookPhase::AskRead {
            bid: head_level(s.bid_head, node_level),
        },
        BookPhase::AskRead { bid } => BookPhase::Emit {
            bid,
            ask: head_level(s.ask_head, node_level),
            word: bits::<2>(0),
        },
        BookPhase::Emit { bid, ask, word } => {
            if word == bits::<2>(3) {
                BookPhase::Idle
            } else {
                BookPhase::Emit {
                    bid,
                    ask,
                    word: word + bits::<2>(1),
                }
            }
        }
    };

    let plu = if loading { loaded } else { q };
    // Advanced in `Load`, after the read of entry `plu_rd_ptr` issued in
    // `Idle` has returned.
    let plu_rd_ptr = if loading {
        s.plu_rd_ptr + bits::<7>(1)
    } else {
        s.plu_rd_ptr
    };
    let slot = if clearing {
        s.slot + bits::<11>(1)
    } else if loading {
        symbol_hash(loaded.symbol)
    } else if collide {
        s.slot + bits::<11>(1)
    } else {
        s.slot
    };
    let bid_head = if hit {
        meta_bid
    } else if vacant {
        bits::<13>(0)
    } else if set_head && !q.sell {
        new_head
    } else {
        s.bid_head
    };
    let ask_head = if hit {
        meta_ask
    } else if vacant {
        bits::<13>(0)
    } else if set_head && q.sell {
        new_head
    } else {
        s.ask_head
    };
    let used = if inserting && !have_free {
        s.used + bits::<13>(1)
    } else {
        s.used
    };
    let free_sp = if deleting {
        s.free_sp + bits::<14>(1)
    } else if inserting && have_free {
        s.free_sp - bits::<14>(1)
    } else {
        s.free_sp
    };
    let bbo_wr_ptr = if emit_last {
        s.bbo_wr_ptr + bits::<9>(1)
    } else {
        s.bbo_wr_ptr
    };

    let updates = bump32(n.updates, loading);
    let bbos = bump32(n.bbos, emit_last);
    let symbols = bump32(n.symbols, vacant);
    let levels = if inserting {
        n.levels + bits::<32>(1)
    } else if deleting {
        n.levels - bits::<32>(1)
    } else {
        n.levels
    };
    let errors = bump32(n.errors, table_full || too_deep || pool_empty);
    let counters = BookCounters {
        updates,
        bbos,
        symbols,
        levels,
        errors,
    };

    let next = BookState {
        phase: next_phase,
        plu,
        plu_rd_ptr,
        slot,
        bid_head,
        ask_head,
        used,
        free_sp,
        bbo_wr_ptr,
        counters,
    };

    // Nodes are read only for `Walk` and the two head reads.
    let node_raddr = match next.phase {
        BookPhase::Walk(c) => c.cur,
        BookPhase::BidRead => next.bid_head,
        BookPhase::AskRead { .. } => next.ask_head,
        _ => bits::<13>(0),
    };
    let bbo_word = match s.phase {
        BookPhase::Emit { bid, ask, word } => [
            q.symbol,
            bid.price,
            ask.price,
            ask.size.concat::<32, 64>(bid.size),
        ]
        .at(word),
        _ => bits::<64>(0),
    };
    // `Walk` writes an inserted node's `next`, or unlinks a deleted node from
    // its predecessor; `Link` points `after.next` at the inserted node.
    let node_next_waddr = match s.phase {
        BookPhase::Link { after, .. } => after,
        _ => {
            if inserting {
                alloc
            } else {
                walk.prev
            }
        }
    };
    let node_next_wdata = match s.phase {
        BookPhase::Link { node, .. } => node,
        _ => {
            if inserting {
                walk.cur
            } else {
                nn
            }
        }
    };

    BookOut {
        state: next,
        plu_raddr: next.plu_rd_ptr.slice::<6>(0),
        sym_raddr: next.slot,
        sym_waddr: s.slot,
        sym_key_we: vacant,
        sym_key_wdata: q.symbol,
        sym_meta_we: clearing || finishing,
        sym_meta_wdata: if clearing {
            bits::<27>(0)
        } else {
            pack_meta(s.bid_head, s.ask_head)
        },
        node_raddr,
        node_waddr: if inserting { alloc } else { walk.cur },
        node_price_we: inserting,
        node_price_wdata: q.price,
        node_size_we: inserting || updating,
        node_size_wdata: q.size,
        node_next_we: inserting || (deleting && !at_head) || linking,
        node_next_waddr,
        node_next_wdata,
        free_raddr: (next.free_sp - bits::<14>(1)).slice::<13>(0),
        free_we: deleting,
        free_waddr: s.free_sp.slice::<13>(0),
        free_wdata: walk.cur,
        bbo_we: emitting,
        bbo_waddr: s.bbo_wr_ptr.slice::<8>(0).concat::<2, 10>(word),
        bbo_wdata: bbo_word,
    }
}
