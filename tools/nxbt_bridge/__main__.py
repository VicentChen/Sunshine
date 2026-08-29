"""Command-line entry point for the local NXBT Bridge service."""

from __future__ import annotations

import argparse
import logging

from .bridge import Bridge, FakeBackend, NxbtBackend, SystemClock, UnixBridgeServer, install_signal_handlers, preflight_bluez


def main() -> None:
    """Run the Bridge with explicit backend, adapter, socket, and watchdog settings."""

    parser = argparse.ArgumentParser(description="NXBT Bridge service")
    parser.add_argument("--backend", choices=("fake", "nxbt"), default="fake")
    parser.add_argument("--socket", default="/run/nxbt-bridge/control.sock")
    parser.add_argument("--adapter-path")
    parser.add_argument("--watchdog-ms", type=int, choices=range(50, 1001), default=150, metavar="50..1000")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    if args.backend == "fake":
        backend = FakeBackend()
    else:
        preflight_bluez()
        backend = NxbtBackend(args.adapter_path)
    server = UnixBridgeServer(Bridge(backend, SystemClock(), args.watchdog_ms * 1_000), args.socket)
    install_signal_handlers(server)
    try:
        server.serve_forever()
    finally:
        server.stop()


if __name__ == "__main__":
    main()
