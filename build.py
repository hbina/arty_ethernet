#!/usr/bin/env python3
"""Build, test, and manage the Arty Ethernet project using Python 3."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
DEFAULT_BITFILE = ROOT / "bitlogic_ethernet/build/bitlogic_ethernet.bit"


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--xilinx-root", type=Path,
                        default=Path(os.environ.get("XILINX_ROOT", Path.home() / "Xilinx/2025.2")))
    common.add_argument("--part", default=os.environ.get("PART", "xc7a100ticsg324-1L"))
    common.add_argument("--hw-port", type=int, default=int(os.environ.get("HW_PORT", "3124")))
    common.add_argument("--dry-run", action="store_true", help="print actions without executing them")

    def command(name, help_text):
        return commands.add_parser(name, help=help_text, parents=[common])

    command("build", "generate the bitlogic RTL and build the Ethernet bitstream")
    command("hw-server", "start the JTAG hardware server in the foreground")
    command("probe", "inspect the JTAG chain using Vivado")
    command("list-targets", "list hardware targets using XSCT")
    program = command("program", "program an existing bitstream onto the board")
    program.add_argument("--bitfile", type=Path, default=DEFAULT_BITFILE)
    program.add_argument("--method", choices=("vivado", "xsct"), default="vivado")
    test = command("test", "run board-independent Python tests by default")
    test.add_argument("--hardware", action="store_true", help="also run tests requiring a board")
    test.add_argument("--program", action="store_true", help="build/program before hardware tests")
    test.add_argument("--no-build", action="store_true", help="reuse the bitstream with --program")
    test.add_argument("--bitfile", type=Path, default=DEFAULT_BITFILE)
    test.add_argument("--iface", default="eno1")
    test.add_argument("--timeout", type=float, default=8.0)
    command("clean", "remove generated build outputs and Vivado logs")
    return cli


def main(argv=None):
    cli = parser()
    args = cli.parse_args(argv)
    if args.command == "test":
        if args.program and not args.hardware:
            cli.error("--program requires --hardware")
        if args.no_build and not args.program:
            cli.error("--no-build requires --program")
    if not 1 <= args.hw_port <= 65535:
        cli.error("--hw-port must be between 1 and 65535")

    env = os.environ.copy()
    env.update(PART=args.part, HW_PORT=str(args.hw_port),
               XILINX_ROOT=str(args.xilinx_root.expanduser().resolve()))

    def tool(product, name):
        return str(Path(env["XILINX_ROOT"]) / product / "bin" / name)

    def run(*cmd):
        cmd = [str(item) for item in cmd]
        print(shlex.join(cmd), flush=True)
        if not args.dry_run:
            subprocess.run(cmd, cwd=ROOT, env=env, check=True)

    def vivado(script, *extra):
        run(tool("Vivado", "vivado"), "-mode", "batch", "-source", script, *extra)

    try:
        if args.command == "build":
            run("cargo", "run", "--quiet", "--release", "--locked",
                "--manifest-path", "bitlogic_ethernet/Cargo.toml",
                "--bin", "generate", "--", "bitlogic_ethernet/build/rtl")
            vivado("bitlogic_ethernet/scripts/build_bitstream.tcl")
        elif args.command == "hw-server":
            run(tool("Vitis", "hw_server"), "-s", f"tcp::{args.hw_port}")
        elif args.command == "probe":
            vivado("scripts/probe_vivado_hw.tcl")
        elif args.command == "list-targets":
            run(tool("Vitis", "xsct"), "scripts/list_targets.tcl")
        elif args.command == "program":
            bitfile = args.bitfile.expanduser().resolve()
            if not args.dry_run and not bitfile.is_file():
                raise FileNotFoundError(f"bitstream does not exist: {bitfile}")
            if args.method == "vivado":
                vivado("scripts/program_vivado_hw.tcl", "-tclargs", bitfile)
            else:
                run(tool("Vitis", "xsct"), "scripts/program_hw.tcl", bitfile)
        elif args.command == "test":
            cmd = [sys.executable, "-m", "pytest"]
            if args.hardware:
                cmd += ["tests/test_build_cli.py", "tests/hw", "--iface", args.iface, "--hw-port", str(args.hw_port),
                        "--bitfile", str(args.bitfile.expanduser().resolve()),
                        "--timeout", str(args.timeout)]
                if args.program:
                    cmd.append("--program")
                if args.no_build:
                    cmd.append("--no-build")
            else:
                cmd += ["tests/test_build_cli.py", "tests/hw/test_ethernet_helpers.py"]
            run(*cmd)
        elif args.command == "clean":
            paths = [ROOT / name for name in (".Xil", "build", "bitlogic_ethernet/build")]
            paths += sorted({path for pattern in ("vivado*.jou", "vivado*.log", "vivado_pid*.str")
                             for path in ROOT.glob(pattern)})
            for path in paths:
                if path.exists() or path.is_symlink():
                    print(f"Remove {path}", flush=True)
                    if not args.dry_run:
                        if path.is_dir() and not path.is_symlink():
                            shutil.rmtree(path)
                        else:
                            path.unlink()
    except subprocess.CalledProcessError as exc:
        print(f"Command failed with exit status {exc.returncode}", file=sys.stderr)
        return exc.returncode if exc.returncode > 0 else 128 - exc.returncode
    except OSError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
