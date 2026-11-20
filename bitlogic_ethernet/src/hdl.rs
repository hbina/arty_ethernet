//! Verilog generation.
//!
//! The Ethernet core comes from the `system!` declaration in [`crate::core`]:
//! the `eth_core` module, one registered unit wrapper per step function, the
//! template ROM and the shared RAM. The LED-domain status unit is a single
//! unit wrapped on its own and instantiated by the handwritten `arty_top.v`.
use crate::{core::eth_core, status};
use bitlogic::Error;

/// Every generated file as `(file name, contents)`.
pub fn generated_files() -> Result<Vec<(String, String)>, Error> {
    let mut files = eth_core::verilog()?;
    files.push((
        "status_unit.v".into(),
        status::status_step::ir()?.to_unit_verilog("status_unit")?,
    ));
    Ok(files)
}
