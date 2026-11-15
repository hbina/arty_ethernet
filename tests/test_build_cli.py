"""Board-independent checks for CLI orchestration and failure handling."""

import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("arty_build", Path(__file__).resolve().parents[1] / "build.py")
cli = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cli)


class BuildCliTests(unittest.TestCase):
    @patch.object(cli.subprocess, "run")
    def test_build_runs_hls_before_vivado_and_passes_configuration(self, run):
        self.assertEqual(cli.main(["build", "--xilinx-root", "/tmp/tools with spaces",
                                   "--part", "test-part"]), 0)
        self.assertEqual(run.call_count, 2)
        first, second = run.call_args_list
        self.assertEqual(first.args[0][0], "/tmp/tools with spaces/Vitis/bin/vitis-run")
        self.assertEqual(second.args[0][-1], "hls_ethernet/scripts/build_hls_bitstream.tcl")
        self.assertEqual(first.kwargs["env"]["PART"], "test-part")
        self.assertEqual(first.kwargs["cwd"], cli.ROOT)

    @patch.object(cli.subprocess, "run")
    def test_failure_stops_before_vivado(self, run):
        run.side_effect = subprocess.CalledProcessError(7, "hls")
        self.assertEqual(cli.main(["build"]), 7)
        self.assertEqual(run.call_count, 1)

    @patch.object(cli.subprocess, "run")
    def test_skip_hls_only_runs_vivado(self, run):
        self.assertEqual(cli.main(["build", "--skip-hls"]), 0)
        self.assertEqual(run.call_count, 1)
        self.assertTrue(run.call_args.args[0][0].endswith("/Vivado/bin/vivado"))

    @patch.object(cli.subprocess, "run")
    def test_dry_run_does_not_execute(self, run):
        for command in (["build"], ["program"], ["test", "--hardware", "--program"]):
            self.assertEqual(cli.main([*command, "--dry-run"]), 0)
        run.assert_not_called()

    @patch.object(cli.subprocess, "run")
    def test_default_tests_exclude_hardware(self, run):
        self.assertEqual(cli.main(["test"]), 0)
        cmd = run.call_args.args[0]
        self.assertIn("tests/hw/test_ethernet_helpers.py", cmd)
        self.assertNotIn("--program", cmd)
        self.assertNotIn("tests", cmd)

    @patch.object(cli.subprocess, "run")
    def test_hardware_test_options_and_environment(self, run):
        self.assertEqual(cli.main(["test", "--hardware", "--program", "--no-build",
                                   "--iface", "enp3s0", "--hw-port", "3125"]), 0)
        cmd = run.call_args.args[0]
        self.assertIn("--program", cmd)
        self.assertIn("--no-build", cmd)
        self.assertIn("enp3s0", cmd)
        self.assertEqual(run.call_args.kwargs["env"]["HW_PORT"], "3125")

    def test_program_requires_hardware_flag(self):
        with self.assertRaises(SystemExit) as error:
            cli.main(["test", "--program"])
        self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
