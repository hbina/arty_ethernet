import os
import socket
import subprocess
import time
from pathlib import Path

import pytest

from .ethernet import RawEthernet


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BITFILE = REPO_ROOT / "hls_ethernet" / "build" / "hls_ethernet.bit"


def pytest_addoption(parser):
    group = parser.getgroup("arty-hardware")
    group.addoption("--iface", default="eno1", help="host Ethernet interface connected to the Arty")
    group.addoption("--program", action="store_true", help="build/program the FPGA before tests")
    group.addoption("--hw-port", default="3124", help="hw_server TCP port for programming")
    group.addoption("--bitfile", default=str(DEFAULT_BITFILE), help="bitstream to program")
    group.addoption("--no-build", action="store_true", help="skip make hls-bit when --program is set")
    group.addoption("--timeout", type=float, default=8.0, help="Ethernet receive timeout in seconds")


def _run(cmd, env=None):
    merged_env = os.environ.copy()
    if env:
        merged_env.update(env)
    subprocess.run(cmd, cwd=REPO_ROOT, env=merged_env, check=True)


def _port_open(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.25)
        return probe.connect_ex(("127.0.0.1", int(port))) == 0


def _start_hw_server(port):
    proc = subprocess.Popen(
        ["make", "hw-server", f"HW_PORT={port}"],
        cwd=REPO_ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            output = proc.stdout.read() if proc.stdout else ""
            raise RuntimeError(f"hw_server exited before accepting connections:\n{output}")
        if _port_open(port):
            return proc
        time.sleep(0.2)
    proc.terminate()
    raise RuntimeError(f"hw_server did not start on port {port} within 10s")


@pytest.fixture(scope="session", autouse=True)
def programmed_board(request):
    if not request.config.getoption("--program"):
        return

    hw_port = request.config.getoption("--hw-port")
    bitfile = Path(request.config.getoption("--bitfile")).expanduser().resolve()
    started_server = None

    if not request.config.getoption("--no-build"):
        _run(["make", "hls-bit"])

    if not bitfile.exists():
        pytest.fail(f"bitfile does not exist: {bitfile}")

    if not _port_open(hw_port):
        started_server = _start_hw_server(hw_port)

    try:
        _run(["make", "program-vivado", f"HW_PORT={hw_port}", f"BITFILE={bitfile}"])
    finally:
        if started_server is not None:
            started_server.terminate()
            try:
                started_server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                started_server.kill()
                started_server.wait(timeout=5)


@pytest.fixture
def timeout(request):
    return request.config.getoption("--timeout")


@pytest.fixture
def raw_eth(request):
    iface = request.config.getoption("--iface")
    try:
        eth = RawEthernet(iface)
    except PermissionError as exc:
        pytest.fail(f"raw Ethernet sockets require sudo or CAP_NET_RAW: {exc}")
    except OSError as exc:
        pytest.fail(f"could not open raw Ethernet socket on interface {iface!r}: {exc}")

    try:
        eth.drain(0.2)
        yield eth
    finally:
        eth.close()
