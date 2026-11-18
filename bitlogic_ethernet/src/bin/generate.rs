//! Write the generated Verilog into the directory given as the only argument.
use std::{env, fs, path::PathBuf, process::ExitCode};

fn main() -> ExitCode {
    let args: Vec<_> = env::args_os().skip(1).collect();
    let [dir] = args.as_slice() else {
        eprintln!("usage: generate <output-directory>");
        return ExitCode::from(2);
    };
    let dir = PathBuf::from(dir);
    let result = (|| -> Result<(), Box<dyn std::error::Error>> {
        fs::create_dir_all(&dir)?;
        for (name, text) in bitlogic_ethernet::hdl::generated_files()? {
            fs::write(dir.join(&name), text)?;
            println!("{}", dir.join(&name).display());
        }
        Ok(())
    })();
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("error: {error}");
            ExitCode::FAILURE
        }
    }
}
