"""Run the Phase 1 NXBT Pro Controller pairing smoke test.

The test assumes BlueZ was configured externally with only ``--compat`` and
``--noplugin=input``.  It deliberately replaces NXBT's legacy automatic BlueZ
management function so the test cannot write a broad plugin override or
restart BlueZ itself.
"""

import argparse
import sys
import time

import nxbt


def main(reconnect: bool, hold_seconds: float) -> int:
    """Create one controller, wait for a Switch, send two inputs, and clean up."""
    bridge = nxbt.Nxbt(disable_logging=True, manage_bluez=False)
    controller_index = None

    try:
        adapters = bridge.get_available_adapters()
        print(f"available adapters: {len(adapters)}", flush=True)
        if not adapters:
            print("result: failed (no adapter)", flush=True)
            return 2

        reconnect_addresses = bridge.get_switch_addresses() if reconnect else None
        if reconnect and not reconnect_addresses:
            print("result: failed (no paired Switch available for reconnect)", flush=True)
            return 5

        controller_index = bridge.create_controller(
            nxbt.PRO_CONTROLLER,
            adapter_path=adapters[0],
            reconnect_address=reconnect_addresses,
        )
        print("controller created; waiting up to 180 seconds", flush=True)
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline:
            state = bridge.state.get(controller_index, {})
            if state.get("state") == "connected":
                bridge.press_buttons(controller_index, [nxbt.Buttons.A])
                bridge.press_buttons(controller_index, [nxbt.Buttons.DPAD_RIGHT])
                if hold_seconds > 0:
                    print(
                        f"result: connected; holding for {hold_seconds:g} seconds",
                        flush=True,
                    )
                    time.sleep(hold_seconds)
                print("result: connected; A and DPAD_RIGHT sent", flush=True)
                return 0
            if state.get("state") == "crashed":
                print("result: failed (controller crashed)", flush=True)
                return 3
            time.sleep(0.1)

        print("result: timeout waiting for Switch", flush=True)
        return 4
    finally:
        if controller_index is not None:
            try:
                bridge.remove_controller(controller_index)
                time.sleep(0.5)
            except Exception:
                print("cleanup: controller removal failed", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--reconnect",
        action="store_true",
        help="Reconnect to the previously paired Switch instead of pairing anew.",
    )
    parser.add_argument(
        "--hold-seconds",
        type=float,
        default=0,
        help="Keep the parent process alive after connecting for crash cleanup testing.",
    )
    arguments = parser.parse_args()
    sys.exit(main(arguments.reconnect, arguments.hold_seconds))
