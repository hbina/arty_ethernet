"""Board- and Vivado-independent checks for scripts/die_map.py."""

import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("die_map", Path(__file__).resolve().parents[1] / "scripts/die_map.py")
die_map = importlib.util.module_from_spec(spec)
spec.loader.exec_module(die_map)

# A 6 x 8 die: a logic column, an interconnect column, a BRAM column, I/O.
DUMP = "\n".join([
    "V\tVivado v2025.2",
    "D\txc7a100ticsg324-1L\tarty_top",
    *(f"T\t0\t{y}\tLIOB33" for y in range(8)),
    *(f"T\t1\t{y}\tINT_L" for y in range(8)),
    *(f"T\t2\t{y}\tCLBLM_L" for y in range(8)),
    *(f"T\t3\t{y}\tCLBLL_R" for y in range(8)),
    "T\t4\t5\tBRAM_L",
    "T\t5\t5\tDSP_R",
    "R\tX0Y0\t0\t0\t5\t7",
    "C\t2\t1\tLUT6\tcore_i/a_i/x_i_1",
    "C\t2\t1\tFDRE\tcore_i/a_i/state_reg[0]",
    "C\t3\t2\tCARRY4\tcore_i/b_i/sum_carry",
    "C\t4\t5\tRAMB36E1\tcore_i/ram_i/mem_reg",
    "C\t3\t4\tFDRE\tsync_reg[0]",
    "P\t0\t3\tclk\tE3",
    "",
])


def page_data(html):
    return json.loads(re.search(r"const DIE=(.*);\n", html).group(1))


class DieMapTests(unittest.TestCase):
    def test_parse_reads_every_record(self):
        d = die_map.parse(DUMP)
        self.assertEqual((d["part"], d["top"], d["version"]), ("xc7a100ticsg324-1L", "arty_top", "Vivado v2025.2"))
        self.assertEqual(len(d["tiles"]), 34)
        self.assertEqual(d["regions"], [("X0Y0", 0, 0, 5, 7)])
        self.assertEqual(len(d["cells"]), 5)
        self.assertEqual(d["pins"], [(0, 3, "clk", "E3")])

    def test_parse_rejects_bad_lines(self):
        with self.assertRaisesRegex(ValueError, "line 2"):
            die_map.parse("V\tx\nT\t1\tnot-a-number\tINT_L\n")
        with self.assertRaisesRegex(ValueError, "no tiles"):
            die_map.parse("V\tx\n")

    def test_tile_categories(self):
        cases = {"CLBLL_L": "L", "CLBLM_R": "M", "BRAM_L": "B", "DSP_R": "D", "LIOB33": "I", "RIOI3": "I",
                 "CMT_TOP_R_UPPER_T": "C", "CLK_BUFG_TOP_R": "C", "GTP_CHANNEL_0": "G", "PCIE_BOT": "P",
                 "PCIE_INT_INTERFACE_L": ".", "PCIE_NULL": ".", "CFG_CENTER_MID": "F", "INT_R": "n", "VBRK": "."}
        for tile, want in cases.items():
            self.assertEqual(die_map.tile_category(tile), want, tile)

    def test_model_groups_cells_by_hierarchy(self):
        m = die_map.model(die_map.parse(DUMP), depth=2)
        self.assertEqual((m["w"], m["h"]), (6, 8))
        self.assertEqual(m["grid"][5], "InMLBD")
        self.assertEqual(m["slices"], 32)
        self.assertEqual(m["placed"], 5)
        # Ties are broken by name, so the order is stable.
        self.assertEqual(m["groups"], ["core_i/a_i", "(top level)", "core_i/b_i", "core_i/ram_i"])
        a = m["groups"].index("core_i/a_i")
        self.assertIn([2, 1, a, 2], m["cells"])
        self.assertEqual(m["summary"][a], {"FF": 1, "LUT": 1})
        self.assertEqual(m["summary"][m["groups"].index("core_i/ram_i")], {"BRAM": 1})
        shallow = die_map.model(die_map.parse(DUMP), depth=1)
        self.assertEqual(shallow["groups"], ["core_i", "(top level)"])

    def test_many_groups_share_other(self):
        lines = [DUMP] + [f"C\t2\t3\tFDRE\tu{k}_i/r_reg" for k in range(12)]
        m = die_map.model(die_map.parse("\n".join(lines)), depth=1)
        self.assertEqual(len(m["groups"]), die_map.GROUP_TOKENS)
        self.assertEqual(m["groups"][-1], "(other)")
        self.assertEqual(sum(n for *_, n in m["cells"]), m["placed"])

    def test_render_is_deterministic_and_self_contained(self):
        d = die_map.parse(DUMP)
        html = die_map.render(d, title="Test <map>")
        self.assertEqual(html, die_map.render(die_map.parse(DUMP), title="Test <map>"))
        self.assertIn("<title>Test &lt;map&gt;</title>", html)
        self.assertNotIn("@", re.sub(r"wght@|@media", "", html))
        self.assertEqual(page_data(html)["placed"], 5)

    def test_render_escapes_script_end_in_names(self):
        d = die_map.parse(DUMP + "P\t0\t4\tx</script>\tA1\n")
        html = die_map.render(d)
        self.assertEqual(html.count("</script>"), 1)
        self.assertIn([0, 4, "x</script>", "A1"], page_data(html)["pins"])

    def test_cli_render_writes_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            dump = Path(tmp) / "d.tsv"
            dump.write_text(DUMP)
            out = Path(tmp) / "sub" / "map.html"
            self.assertEqual(die_map.main(["render", str(dump), str(out)]), 0)
            self.assertIn("arty_top on xc7a100ticsg324-1L", out.read_text())
            self.assertEqual(die_map.main(["render", str(Path(tmp) / "missing.tsv"), str(out)]), 1)


if __name__ == "__main__":
    unittest.main()
