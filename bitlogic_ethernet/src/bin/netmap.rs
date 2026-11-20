//! Experiment: map each unit's step function with bitlogic-net, report the
//! numbers, check the mapped netlist against the IR on random inputs, and
//! optionally write unit files whose `<unit>_logic` is the mapped netlist.
//!
//! usage: netmap [<output-directory>]
use bitlogic::Logic;
use bitlogic_ethernet::{proto, rx, status, tx};
use bitlogic_net::{Net, map};
use std::{env, fs, path::PathBuf, time::Instant};

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

/// Check `net`'s mapping against the IR interpreter on 64 * `rounds` random
/// inputs.
fn check(logic: &Logic, mapped: &bitlogic_net::Mapped, rounds: usize, seed: u64) {
    let mut rng = Rng(seed);
    for _ in 0..rounds {
        let vectors: Vec<Vec<u64>> = (0..64)
            .map(|_| {
                logic
                    .inputs()
                    .iter()
                    .map(|p| rng.next() & (u64::MAX >> (64 - p.width)))
                    .collect()
            })
            .collect();
        let mut words = Vec::new();
        for (k, p) in logic.inputs().iter().enumerate() {
            for bit in 0..p.width {
                words.push(
                    vectors
                        .iter()
                        .enumerate()
                        .fold(0u64, |w, (lane, v)| w | (v[k] >> bit & 1) << lane),
                );
            }
        }
        let got = mapped.simulate(&words);
        for (lane, v) in vectors.iter().enumerate() {
            let want = logic.evaluate(v).unwrap();
            for (port, (bits, w)) in got.iter().zip(&want).enumerate() {
                let value = bits
                    .iter()
                    .enumerate()
                    .fold(0u64, |a, (b, word)| a | (word >> lane & 1) << b);
                assert_eq!(value, *w, "output {port} lane {lane}");
            }
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let out = env::args_os().nth(1).map(PathBuf::from);
    let units: [(&str, Logic); 4] = [
        ("rx_unit", rx::rx_step::ir()?),
        ("proto_unit", proto::proto_step::ir()?),
        ("tx_unit", tx::tx_step::ir()?),
        ("status_unit", status::status_step::ir()?),
    ];
    println!(
        "{:<12} {:>7} {:>11} {:>7} {:>6} {:>6} | {:<16} {:>6} {:>5} {:>6} {:>6} {:>5}",
        "unit",
        "ir",
        "ands/xors",
        "carry",
        "sums",
        "depth",
        "passes",
        "LUTs",
        "route",
        "CARRY4",
        "levels",
        "ms"
    );
    for (name, logic) in &units {
        let t = Instant::now();
        let net = Net::from_logic(logic)?.cleanup();
        let s = net.stats();
        let variants: Vec<(&str, Net)> = vec![
            ("none", net.clone()),
            ("balance", net.balance()),
            ("lower<3", net.lower_short_chains(3)),
            ("lower<3+balance", net.lower_short_chains(3).balance()),
            ("lower<5+balance", net.lower_short_chains(5).balance()),
        ];
        let mut best: Option<(usize, String, bitlogic_net::Mapped)> = None;
        for (label, n) in variants {
            let t = Instant::now();
            let mapped = map(&n);
            let ms = t.elapsed().as_millis();
            let st = mapped.stats();
            println!(
                "{:<12} {:>7} {:>5}/{:<5} {:>7} {:>6} {:>6} | {:<16} {:>6} {:>5} {:>6} {:>6} {:>5}",
                name,
                logic.nodes().len(),
                s.ands,
                s.xors,
                s.carries,
                s.sums,
                s.depth,
                label,
                st.luts,
                st.route_luts,
                st.carry4,
                st.depth,
                ms
            );
            // Rank by LUTs, then levels.
            let score = st.luts * 1000 + st.depth;
            if best.as_ref().is_none_or(|b| score < b.0) {
                best = Some((score, label.to_string(), mapped));
            }
        }
        let (_, label, mapped) = best.unwrap();
        check(logic, &mapped, 40, 0x1234_5678 ^ logic.nodes().len() as u64);
        println!(
            "{name}: picked {label}; 2,560 random inputs match the IR ({} ms total)",
            t.elapsed().as_millis()
        );
        if let Some(dir) = &out {
            fs::create_dir_all(dir)?;
            let inner = format!("{name}_logic");
            let unit = logic.to_unit_verilog(name)?;
            let text = unit.replace(&logic.to_verilog(&inner)?, &mapped.to_verilog(&inner));
            assert_ne!(text, unit, "inner module replaced");
            fs::write(dir.join(format!("{name}.v")), text)?;
        }
    }
    Ok(())
}
