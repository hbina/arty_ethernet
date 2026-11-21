#!/usr/bin/env python3
"""Build, test, and program the Arty A7 projects under projects/.

Each project is a Cargo crate in projects/<name>/ with a `generate` bin that
writes its Verilog into the directory it is given, handwritten Verilog in
rtl/, constraints in constraints/*.xdc, and its Vivado top module in
Cargo.toml under [package.metadata.arty] (top = "..."). Outputs go to
build/<name>/, the bitstream to build/<name>/<name>.bit."""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parent
PROJECTS = ROOT / "projects"


def projects():
    """Names of the projects: directories under projects/ with a Cargo.toml."""
    if not PROJECTS.is_dir():
        return []
    return sorted(p.name for p in PROJECTS.iterdir() if (p / "Cargo.toml").is_file())


def project_top(name):
    manifest = PROJECTS / name / "Cargo.toml"
    with manifest.open("rb") as f:
        meta = tomllib.load(f).get("package", {}).get("metadata", {}).get("arty", {})
    if "top" not in meta:
        raise ValueError(f"{manifest}: missing [package.metadata.arty] top = \"<module>\"")
    return meta["top"]


def parser():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--xilinx-root", type=Path,
                        default=Path(os.environ.get("XILINX_ROOT", Path.home() / "Xilinx/2025.2")))
    common.add_argument("--part", default=os.environ.get("PART", "xc7a100ticsg324-1L"))
    common.add_argument("--hw-port", type=int, default=int(os.environ.get("HW_PORT", "3124")))
    common.add_argument("--dry-run", action="store_true", help="print actions without executing them")

    def command(name, help_text, project=False):
        sub = commands.add_parser(name, help=help_text, parents=[common])
        if project:
            sub.add_argument("project", nargs="?",
                             help="project under projects/ (default: the only one, if there is one)")
        return sub

    command("projects", "list the projects")
    command("build", "generate a project's RTL and build its bitstream", project=True)
    command("hw-server", "start the JTAG hardware server in the foreground")
    command("probe", "inspect the JTAG chain using Vivado")
    command("list-targets", "list hardware targets using XSCT")
    program = command("program", "program an existing bitstream onto the board", project=True)
    program.add_argument("--bitfile", type=Path, help="default: build/<project>/<project>.bit")
    program.add_argument("--method", choices=("vivado", "xsct"), default="vivado")
    test = command("test", "run board-independent Python tests by default", project=True)
    test.add_argument("--hardware", action="store_true", help="also run tests requiring a board")
    test.add_argument("--program", action="store_true", help="build/program before hardware tests")
    test.add_argument("--no-build", action="store_true", help="reuse the bitstream with --program")
    test.add_argument("--bitfile", type=Path, help="default: build/<project>/<project>.bit")
    test.add_argument("--iface", default="eno1")
    test.add_argument("--timeout", type=float, default=8.0)
    command("clean", "remove generated build outputs (all, or one project's) and Vivado logs",
            project=True)
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

    known = projects()
    if getattr(args, "project", None) is not None:
        if args.project not in known:
            cli.error(f"unknown project {args.project!r}; projects: {', '.join(known) or 'none'}")
    elif hasattr(args, "project") and args.command != "clean":
        if len(known) != 1:
            cli.error(f"name a project; projects: {', '.join(known) or 'none'}")
        args.project = known[0]
    project = getattr(args, "project", None)
    if project is not None:
        project_dir = Path("projects") / project
        out_dir = Path("build") / project
        if getattr(args, "bitfile", "absent") is None:
            args.bitfile = ROOT / out_dir / f"{project}.bit"

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
        if args.command == "projects":
            for name in known:
                print(name)
        elif args.command == "build":
            top = project_top(project)
            rtl = out_dir / "rtl"
            # Start from an empty directory so no stale generated file reaches Vivado.
            if not args.dry_run and (ROOT / rtl).exists():
                shutil.rmtree(ROOT / rtl)
            run("cargo", "run", "--quiet", "--release", "--locked",
                "--manifest-path", project_dir / "Cargo.toml",
                "--bin", "generate", "--", rtl)
            vivado("scripts/build_bitstream.tcl", "-tclargs", project_dir, out_dir, top, project)
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
            cmd = [sys.executable, "-m", "pytest", "tests"]
            hw_tests = project_dir / "tests" / "hw"
            if (ROOT / hw_tests).is_dir():
                cmd.append(hw_tests)
            if args.hardware:
                cmd += ["--iface", args.iface, "--hw-port", str(args.hw_port),
                        "--bitfile", str(args.bitfile.expanduser().resolve()),
                        "--timeout", str(args.timeout)]
                if args.program:
                    cmd.append("--program")
                if args.no_build:
                    cmd.append("--no-build")
            else:
                # Tests that touch the board carry the `hardware` marker.
                cmd += ["-m", "not hardware"]
            run(*cmd)
        elif args.command == "clean":
            build = ROOT / "build" / project if project else ROOT / "build"
            paths = [ROOT / ".Xil", build]
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
