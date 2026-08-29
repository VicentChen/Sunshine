"""Tests for the independent NXBT Bridge state machine and Unix socket setup."""

from pathlib import Path
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

from tools.nxbt_bridge.bridge import Bridge, FakeBackend, NxbtBackend, UnixBridgeServer, validate_bluez_exec_start
from tools.nxbt_bridge.protocol import ControllerState, ControllerStatus, Message, MessageType, ProtocolError, decode_message, encode_message


class FakeClock:
    """Mutable monotonic clock that makes watchdog tests deterministic."""

    def __init__(self, now_us=0):
        """Start the clock at a caller-selected monotonic timestamp."""

        self.now = now_us

    def now_us(self):
        """Return the current deterministic monotonic timestamp."""

        return self.now


class StubNxbt:
    """In-memory NXBT substitute that records the bridge-facing API calls."""

    def __init__(self, manage_bluez):
        """Initialize observable controller and input state."""

        self.manage_bluez = manage_bluez
        self.state = {}
        self.created = []
        self.switch_addresses = ["00:11:22:33:44:55"]
        self.inputs = []
        self.removed = []
        self.closed = False

    def create_controller(self, controller_type, adapter_path=None, reconnect_address=None):
        """Record a Pro Controller creation and return a fixed NXBT index."""

        self.created.append((controller_type, adapter_path, reconnect_address))
        self.state[7] = {"state": "connecting"}
        return 7

    def get_switch_addresses(self):
        """Return one fake previously paired Switch address."""

        return self.switch_addresses

    def create_input_packet(self):
        """Return an NXBT-compatible neutral direct-input dictionary."""

        return {name: False for name in ("DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "PLUS", "MINUS", "L", "R", "HOME", "CAPTURE", "Y", "X", "B", "A", "ZL", "ZR")} | {"L_STICK": {"PRESSED": False, "X_VALUE": 0, "Y_VALUE": 0}, "R_STICK": {"PRESSED": False, "X_VALUE": 0, "Y_VALUE": 0}}

    def set_controller_input(self, index, packet):
        """Record the exact NXBT direct-input packet for assertions."""

        self.inputs.append((index, packet))

    def remove_controller(self, index):
        """Record controller removal."""

        self.removed.append(index)

    def _on_exit(self):
        """Record manager-process shutdown."""

        self.closed = True


class StubNxbtModule:
    """Module-like dependency injection target for ``NxbtBackend`` tests."""

    PRO_CONTROLLER = object()
    instance = None

    @classmethod
    def Nxbt(cls, manage_bluez):
        """Create and retain one observable NXBT runtime substitute."""

        cls.instance = StubNxbt(manage_bluez)
        return cls.instance


class BridgeTest(unittest.TestCase):
    """Verify protocol lifecycle, bounded state, failures, and cleanup."""

    def setUp(self):
        """Create a Bridge with observable backend events and fake time."""

        self.clock = FakeClock(1_000)
        self.backend = FakeBackend()
        self.bridge = Bridge(self.backend, self.clock)
        self.client = object()

    def _send(self, message, client=None):
        """Send one encoded message and decode the bridge's direct replies."""

        replies = self.bridge.handle_packet(self.client if client is None else client, encode_message(message))
        return [decode_message(reply) for reply in replies]

    def _attach(self, controller_id=0, status=ControllerStatus.CONNECTED):
        """Complete handshake and attach a fake connected controller slot."""

        self.assertEqual(self._send(Message(MessageType.HELLO)), [Message(MessageType.HELLO_ACK)])
        replies = self._send(Message(MessageType.ATTACH, controller_id=controller_id, client_relative_id=4))
        self.assertEqual(replies, [Message(MessageType.STATUS, controller_id=controller_id, status=status)])

    def test_lifecycle_applies_latest_state_then_neutralizes_and_detaches(self):
        """Run hello, attach, rebind, state, neutralize, and detach in order."""

        self._attach()
        self.assertEqual(self._send(Message(MessageType.REBIND, controller_id=0, client_relative_id=8)), [])
        state = ControllerState(0, 0x20, 64, 65, 1, 2, 3, 4, 0, 1_000)
        self.assertEqual(self._send(Message(MessageType.STATE, state=state)), [])
        self.assertEqual(self.backend.events, [("attach", 0, None)])
        self.assertEqual(self.bridge.flush_states(), [])
        self.assertEqual(self.backend.events[-1], ("state", 0, state))
        self.assertEqual(self._send(Message(MessageType.NEUTRALIZE, controller_id=0)), [])
        self.assertEqual(self._send(Message(MessageType.DETACH, controller_id=0)), [])
        self.assertEqual([event[0] for event in self.backend.events], ["attach", "state", "neutralize", "detach"])

    def test_rejects_malformed_and_unnegotiated_or_duplicate_ownership(self):
        """Reject bad packets, requests before hello, and a competing slot owner."""

        self.assertEqual(self._send(Message(MessageType.ATTACH, controller_id=0)), [Message(MessageType.ERROR, error=ProtocolError.UNSUPPORTED_VERSION)])
        self.assertEqual(self.bridge.handle_packet(self.client, b"bad"), [encode_message(Message(MessageType.ERROR, error=ProtocolError.TRUNCATED))])
        self._attach()
        other_client = object()
        self.assertEqual(self._send(Message(MessageType.HELLO), other_client), [Message(MessageType.HELLO_ACK)])
        self.assertEqual(self._send(Message(MessageType.ATTACH, controller_id=0), other_client), [Message(MessageType.ERROR, error=ProtocolError.INVALID_LENGTH)])

    def test_burst_keeps_only_latest_state_and_discards_old_or_stale_updates(self):
        """Flush a state burst once, retaining only its newest sequence and timestamp."""

        self._attach()
        for sequence in range(1_000):
            state = ControllerState(0, sequence, 0, 0, 0, 0, 0, 0, sequence, sequence)
            self.assertEqual(self._send(Message(MessageType.STATE, state=state)), [])
        self.bridge.flush_states()
        state_events = [event for event in self.backend.events if event[0] == "state"]
        self.assertEqual(len(state_events), 1)
        self.assertEqual(state_events[0][2].sequence, 999)
        self.assertEqual(self._send(Message(MessageType.STATE, state=ControllerState(0, 1, 0, 0, 0, 0, 0, 0, 998, 998))), [])
        self.assertEqual(self._send(Message(MessageType.STATE, state=ControllerState(0, 2, 0, 0, 0, 0, 0, 0, 1_000, 998))), [])
        self.bridge.flush_states()
        self.assertEqual(len([event for event in self.backend.events if event[0] == "state"]), 1)

    def test_watchdog_neutralizes_once_after_timeout_and_ping_refreshes_it(self):
        """Use the fake clock to verify the 150 ms watchdog boundary without sleeps."""

        self._attach()
        state = ControllerState(0, 1, 0, 0, 0, 0, 0, 0, 1, 1)
        self._send(Message(MessageType.STATE, state=state))
        self.bridge.flush_states()
        self.clock.now += 149_000
        self.bridge.watchdog()
        self.assertNotIn("neutralize", [event[0] for event in self.backend.events])
        self._send(Message(MessageType.PING, monotonic_timestamp_us=9))
        self.clock.now += 149_000
        self.bridge.watchdog()
        self.assertNotIn("neutralize", [event[0] for event in self.backend.events])
        self.clock.now += 2_000
        self.bridge.watchdog()
        self.bridge.watchdog()
        self.assertEqual([event[0] for event in self.backend.events].count("neutralize"), 1)

    def test_reports_controller_status_transitions_once(self):
        """Poll backend state changes without repeating an unchanged status."""

        self.backend.statuses[0] = ControllerStatus.CONNECTING
        self._attach(status=ControllerStatus.CONNECTING)
        self.backend.statuses[0] = ControllerStatus.CONNECTED
        replies = self.bridge.poll_statuses()
        self.assertEqual(
            [decode_message(reply) for _owner, reply in replies],
            [Message(MessageType.STATUS, controller_id=0, status=ControllerStatus.CONNECTED)],
        )
        self.assertEqual(self.bridge.poll_statuses(), [])
        self.backend.statuses[0] = ControllerStatus.RECONNECTING
        replies = self.bridge.poll_statuses()
        self.assertEqual(decode_message(replies[0][1]).status, ControllerStatus.RECONNECTING)

    def test_disconnect_shutdown_and_backend_failure_are_safe(self):
        """Neutralize on disconnect/shutdown and report a fake backend exception."""

        self._attach()
        state = ControllerState(0, 1, 0, 0, 0, 0, 0, 0, 1, 1)
        self._send(Message(MessageType.STATE, state=state))
        self.backend.fail_next_apply = True
        replies = self.bridge.flush_states()
        self.assertEqual([decode_message(reply) for _owner, reply in replies], [Message(MessageType.STATUS, controller_id=0, status=ControllerStatus.FAILED)])
        self._send(Message(MessageType.STATE, state=ControllerState(0, 2, 0, 0, 0, 0, 0, 0, 2, 2)))
        self.bridge.flush_states()
        self.bridge.disconnect(self.client)
        self.assertEqual([event[0] for event in self.backend.events][-2:], ["neutralize", "detach"])
        self.bridge.shutdown()
        self.assertEqual(self.backend.events[-1][0], "close")


class UnixBridgeServerTest(unittest.TestCase):
    """Verify restricted socket creation and stale-socket recovery."""

    def test_recovers_stale_socket_with_restricted_permissions(self):
        """Replace only a stale socket, retain its directory, and use mode 0660."""

        with tempfile.TemporaryDirectory() as directory:
            socket_path = Path(directory) / "control.sock"
            stale = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            stale.bind(str(socket_path))
            stale.close()
            server = UnixBridgeServer(Bridge(FakeBackend(), FakeClock()), str(socket_path))
            server.bind()
            self.assertTrue(socket_path.is_socket())
            self.assertEqual(stat.S_IMODE(socket_path.stat().st_mode), 0o660)
            server.stop()
            self.assertFalse(socket_path.exists())

    def test_refuses_to_replace_a_non_socket_path(self):
        """Protect an unrelated file at the configured Unix socket path."""

        with tempfile.TemporaryDirectory() as directory:
            socket_path = Path(directory) / "control.sock"
            socket_path.write_text("keep")
            server = UnixBridgeServer(Bridge(FakeBackend(), FakeClock()), str(socket_path))
            with self.assertRaisesRegex(RuntimeError, "non-socket"):
                server.bind()
            self.assertEqual(socket_path.read_text(), "keep")

    def test_sigterm_neutralizes_and_removes_the_socket(self):
        """Use the fake service subprocess to verify graceful SIGTERM cleanup."""

        with tempfile.TemporaryDirectory() as directory:
            socket_path = Path(directory) / "control.sock"
            process = subprocess.Popen([sys.executable, "-m", "tools.nxbt_bridge", "--backend=fake", f"--socket={socket_path}"])
            try:
                for _ in range(100):
                    if socket_path.is_socket():
                        break
                    time.sleep(0.01)
                self.assertTrue(socket_path.is_socket())
                process.terminate()
                self.assertEqual(process.wait(timeout=2), 0)
                self.assertFalse(socket_path.exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=2)

    def test_packet_socket_handshake_burst_and_disconnect_cleanup(self):
        """Exercise the packet socket with a burst and a client-side disconnect."""

        with tempfile.TemporaryDirectory() as directory:
            socket_path = Path(directory) / "control.sock"
            backend = FakeBackend()
            server = UnixBridgeServer(Bridge(backend, FakeClock()), str(socket_path))
            server.bind()
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            client = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            client.settimeout(1)
            try:
                client.connect(str(socket_path))
                client.sendall(encode_message(Message(MessageType.HELLO)))
                self.assertEqual(decode_message(client.recv(4096)), Message(MessageType.HELLO_ACK))
                client.sendall(encode_message(Message(MessageType.ATTACH, controller_id=0)))
                self.assertEqual(decode_message(client.recv(4096)), Message(MessageType.STATUS, controller_id=0, status=ControllerStatus.CONNECTED))
                for sequence in range(100):
                    client.sendall(encode_message(Message(MessageType.STATE, state=ControllerState(0, sequence, 0, 0, 0, 0, 0, 0, sequence, sequence))))
                for _ in range(100):
                    state_events = [event for event in backend.events if event[0] == "state"]
                    if state_events and state_events[-1][2].sequence == 99:
                        break
                    time.sleep(0.01)
                self.assertEqual(state_events[-1][2].sequence, 99)
            finally:
                client.close()
                for _ in range(100):
                    if "detach" in [event[0] for event in backend.events]:
                        break
                    time.sleep(0.01)
                server.request_stop()
                thread.join(timeout=2)
                self.assertFalse(thread.is_alive())
            self.assertEqual([event[0] for event in backend.events][-3:-1], ["neutralize", "detach"])


class NxbtBackendTest(unittest.TestCase):
    """Verify the production backend's NXBT boundary without Bluetooth hardware."""

    def test_uses_manage_bluez_false_and_converts_complete_state(self):
        """Require the safe NXBT option and map buttons, axes, neutralize, and close."""

        backend = NxbtBackend("/adapter0", StubNxbtModule)
        runtime = StubNxbtModule.instance
        self.assertFalse(runtime.manage_bluez)
        self.assertEqual(backend.attach(2), ControllerStatus.CONNECTING)
        self.assertEqual(runtime.created, [(StubNxbtModule.PRO_CONTROLLER, "/adapter0", ["00:11:22:33:44:55"])])
        state = ControllerState(2, (1 << 0) | (1 << 7) | (1 << 15) | (1 << 16), 0, 0, -32768, 32767, -1, 1, 1, 1)
        backend.apply_state(state)
        packet = runtime.inputs[-1][1]
        self.assertTrue(packet["DPAD_UP"])
        self.assertTrue(packet["R_STICK"]["PRESSED"])
        self.assertTrue(packet["A"])
        self.assertTrue(packet["ZL"])
        self.assertEqual((packet["L_STICK"]["X_VALUE"], packet["L_STICK"]["Y_VALUE"], packet["R_STICK"]["X_VALUE"], packet["R_STICK"]["Y_VALUE"]), (-100, 100, 0, 0))
        backend.neutralize(2)
        self.assertFalse(runtime.inputs[-1][1]["A"])
        backend.detach(2)
        self.assertEqual(runtime.removed, [7])
        backend.close()
        self.assertTrue(runtime.closed)

    def test_falls_back_to_pairing_without_a_previous_switch(self):
        """Omit a reconnect target when BlueZ has no prior Switch record."""

        backend = NxbtBackend("/adapter0", StubNxbtModule)
        runtime = StubNxbtModule.instance
        runtime.switch_addresses = []
        self.assertEqual(backend.attach(0), ControllerStatus.CONNECTING)
        self.assertEqual(runtime.created, [(StubNxbtModule.PRO_CONTROLLER, "/adapter0", None)])
        backend.close()


class BluezPreflightTest(unittest.TestCase):
    """Verify that deployment validation accepts only the narrow BlueZ override."""

    def test_accepts_minimal_input_plugin_exclusion(self):
        """Accept compatibility mode with only the input plugin excluded."""

        validate_bluez_exec_start("/usr/lib/bluetooth/bluetoothd --compat --noplugin=input")

    def test_rejects_missing_or_wildcard_plugin_configuration(self):
        """Reject stock BlueZ arguments and NXBT's unsafe historical wildcard."""

        with self.assertRaisesRegex(RuntimeError, "must be installed"):
            validate_bluez_exec_start("/usr/lib/bluetooth/bluetoothd")
        with self.assertRaisesRegex(RuntimeError, "wildcard"):
            validate_bluez_exec_start("/usr/lib/bluetooth/bluetoothd --compat --noplugin=*")


if __name__ == "__main__":
    unittest.main()
