#!/usr/bin/env python3
"""Render the data-path flowchart in bitlogic_ethernet/ARCHITECTURE.md as one
self-contained HTML page, using the offline diagram-html viewer (sibling
checkout ../diagram-html) with the flowchart preloaded.

    python3 scripts/render_architecture.py [--viewer ../diagram-html] [--out PATH]

The output (default bitlogic_ethernet/build/architecture.html) has the viewer's
CSS and JavaScript inlined, so it opens from disk with no network access.
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DOC = REPO / "bitlogic_ethernet" / "ARCHITECTURE.md"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--viewer", type=Path, default=REPO.parent / "diagram-html")
    parser.add_argument("--out", type=Path, default=REPO / "bitlogic_ethernet" / "build" / "architecture.html")
    args = parser.parse_args()

    match = re.search(r"```mermaid\n(flowchart.*?)```", DOC.read_text(), re.S)
    if not match:
        sys.exit(f"no mermaid flowchart in {DOC}")
    source = match.group(1)

    html = (args.viewer / "index.html").read_text()
    css = (args.viewer / "index.css").read_text()
    js = (args.viewer / "index.js").read_text()
    preload = "source.value = example;"
    if preload not in js:
        sys.exit(f"{args.viewer / 'index.js'}: preload hook {preload!r} not found")
    # Escape "</" so the flowchart text cannot close the inline script.
    js = js.replace(preload, "source.value = " + json.dumps(source).replace("</", "<\\/") + ";", 1)

    for old, new in [
        ('<link rel="stylesheet" href="index.css">', "<style>\n" + css + "\n</style>"),
        ('<script src="index.js"></script>', "<script>\n" + js + "\n</script>"),
        ("<title>Flowchart · Offline</title>", "<title>IEX Feed Architecture</title>"),
    ]:
        if old not in html:
            sys.exit(f"{args.viewer / 'index.html'}: expected {old!r}")
        html = html.replace(old, new, 1)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(html)
    print(f"wrote {args.out} ({len(source.splitlines())} flowchart lines from {DOC.name})")


if __name__ == "__main__":
    main()
