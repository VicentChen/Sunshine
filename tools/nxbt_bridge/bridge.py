"""Independent NXBT Bridge service with bounded state and a fake backend."""

from __future__ import annotations

from dataclasses import dataclass, field
import logging
import os
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import threading
import time
from typing import Protocol

from .protocol import ControllerState, ControllerStatus, Message, MessageType, ProtocolError, decode_message, encode_message, sequence_is_newer


LOGGER = logging.getLogger(__name__)
DEFAULT_SOCKET_PATH = "/run/nxbt-bridge/control.sock"
DEFAULT_WATCHDOG_US = 150_000


def validate_bluez_exec_start(exec_start: str) -> None:
    """Validate the deployment-time BlueZ arguments without changing the host.

    Args:
        exec_start: Effective ``bluetooth.service`` ExecStart representation.
    Raises:
        RuntimeError: If the approved minimal arguments are absent or the unsafe
            wildcard plugin exclusion is present.
    """

    normalized = " ".join(exec_start.split())
    if "--noplugin=*" in normalized:
        raise RuntimeError("unsafe BlueZ wildcard plugin exclusion is configured")
    if "--compat" not in normalized or "--noplugin=input" not in normalized:
        raise RuntimeError("BlueZ must be installed with --compat --noplugin=input before NXBT starts")


def preflight_bluez() -> None:
    """Read and validate the effective BlueZ service command without mutation.

    Raises:
        RuntimeError: If systemd cannot report the service or its command is not
            the minimal configuration approved for NXBT.
    """

    result = subprocess.run(
        ["systemctl", "show", "bluetooth.service", "--property=ExecStart", "--value"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError("unable to read bluetooth.service configuration")
    validate_bluez_exec_start(result.stdout)


class Clock(Protocol):
    """Provide monotonic microseconds to the bridge state machine."""

    def now_us(self) -> int:
        """Return the current monotonic timestamp in microseconds."""


class Backend(Protocol):
    """Own the Bluetooth-facing controller implementation."""

    def attach(self, controller_id: int) -> ControllerStatus:
        """Create or recover the controller backing one bridge slot."""

    def apply_state(self, state: ControllerState) -> None:
        """Apply the newest complete controller state."""

    def status(self, controller_id: int) -> ControllerStatus:
        """Return the current connection state for one controller."""

    def neutralize(self, controller_id: int) -> None:
        """Release all input for one controller immediately."""

    def detach(self, controller_id: int) -> None:
        """Release one controller and its Bluetooth resources."""

    def close(self) -> None:
        """Release all backend resources after neutralization."""


@dataclass
class ControllerRecord:
    """Latest accepted state and ownership for a Bridge controller slot."""

    owner: object
    client_relative_id: int
    latest_state: ControllerState = field(default_factory=ControllerState)
    last_timestamp_us: int = 0
    last_activity_us: int = 0
    accepted_state: bool = False
    dirty: bool = False
    neutral: bool = True
    status: ControllerStatus = ControllerStatus.UNAVAILABLE


class SystemClock:
    """Production monotonic clock implementation."""

    def now_us(self) -> int:
        """Return the host monotonic time in microseconds."""

        return time.monotonic_ns() // 1_000


class FakeBackend:
    """Observable backend used by IPC and watchdog tests without Bluetooth."""

    def __init__(self) -> None:
        """Initialize empty call records and optional failure injection."""

        self.events: list[tuple[str, int, ControllerState | None]] = []
        self.fail_next_apply = False
        self.statuses: dict[int, ControllerStatus] = {}

    def attach(self, controller_id: int) -> ControllerStatus:
        """Record controller creation and report an immediately connected fake."""

        self.events.append(("attach", controller_id, None))
        return self.statuses.setdefault(controller_id, ControllerStatus.CONNECTED)

    def status(self, controller_id: int) -> ControllerStatus:
        """Return the configurable fake connection state.

        Args:
            controller_id: Bridge-owned controller slot.
        Returns:
            Current fake status, defaulting to connected.
        """

        return self.statuses.get(controller_id, ControllerStatus.CONNECTED)

    def apply_state(self, state: ControllerState) -> None:
        """Record one state or raise the configured one-shot failure."""

        if self.fail_next_apply:
            self.fail_next_apply = False
            raise OSError("configured fake backend failure")
        self.events.append(("state", state.controller_id, state))

    def neutralize(self, controller_id: int) -> None:
        """Record a neutral-input request."""

        self.events.append(("neutralize", controller_id, None))

    def detach(self, controller_id: int) -> None:
        """Record controller resource release."""

        self.events.append(("detach", controller_id, None))

    def close(self) -> None:
        """Record backend shutdown."""

        self.events.append(("close", -1, None))


class NxbtBackend:
    """NXBT-backed controller implementation that never manages BlueZ itself."""

    def __init__(self, adapter_path: str | None = None, nxbt_module: object | None = None) -> None:
        """Create the NXBT owner with BlueZ management explicitly disabled.

        Args:
            adapter_path: Optional D-Bus path for the approved Bluetooth adapter.
            nxbt_module: Injectable module used by no-hardware unit tests.
        Raises:
            RuntimeError: If the local NXBT package cannot be imported.
        """

        if nxbt_module is None:
            try:
                import nxbt as nxbt_module
            except ImportError as error:
                raise RuntimeError("NXBT is unavailable; install the configured NXBT environment") from error
        self._module = nxbt_module
        self._adapter_path = adapter_path
        self._nxbt = nxbt_module.Nxbt(manage_bluez=False)
        self._controllers: dict[int, int] = {}

    def attach(self, controller_id: int) -> ControllerStatus:
        """Create a Pro Controller for the requested logical slot.

        Args:
            controller_id: Bridge-owned controller slot.
        Returns:
            Current NXBT connection state mapped to protocol status.
        """

        if controller_id not in self._controllers:
            reconnect_addresses = self._nxbt.get_switch_addresses()
            self._controllers[controller_id] = self._nxbt.create_controller(
                self._module.PRO_CONTROLLER,
                adapter_path=self._adapter_path,
                reconnect_address=reconnect_addresses or None,
            )
        return self._status(controller_id)

    def apply_state(self, state: ControllerState) -> None:
        """Translate one complete bridge snapshot to NXBT direct input.

        Args:
            state: Latest accepted controller state.
        Raises:
            OSError: If the controller slot has no active NXBT controller.
        """

        index = self._controller_index(state.controller_id)
        packet = self._nxbt.create_input_packet()
        flags = state.button_flags
        for flag, name in ((0, "DPAD_UP"), (1, "DPAD_DOWN"), (2, "DPAD_LEFT"), (3, "DPAD_RIGHT"), (4, "PLUS"), (5, "MINUS"), (8, "L"), (9, "R"), (10, "HOME"), (11, "CAPTURE"), (12, "Y"), (13, "X"), (14, "B"), (15, "A"), (16, "ZL"), (17, "ZR")):
            packet[name] = bool(flags & (1 << flag))
        packet["L_STICK"]["PRESSED"] = bool(flags & (1 << 6))
        packet["R_STICK"]["PRESSED"] = bool(flags & (1 << 7))
        packet["L_STICK"]["X_VALUE"] = self._axis_percent(state.left_stick_x)
        packet["L_STICK"]["Y_VALUE"] = self._axis_percent(state.left_stick_y)
        packet["R_STICK"]["X_VALUE"] = self._axis_percent(state.right_stick_x)
        packet["R_STICK"]["Y_VALUE"] = self._axis_percent(state.right_stick_y)
        self._nxbt.set_controller_input(index, packet)

    def status(self, controller_id: int) -> ControllerStatus:
        """Return the current NXBT connection state for one Bridge slot.

        Args:
            controller_id: Bridge-owned controller slot.
        Returns:
            Current protocol connection status.
        """

        return self._status(controller_id)

    def neutralize(self, controller_id: int) -> None:
        """Replace a controller's direct input with NXBT's all-released packet.

        Args:
            controller_id: Bridge-owned controller slot.
        """

        index = self._controller_index(controller_id)
        self._nxbt.set_controller_input(index, self._nxbt.create_input_packet())

    def detach(self, controller_id: int) -> None:
        """Release the NXBT controller assigned to a logical slot.

        Args:
            controller_id: Bridge-owned controller slot.
        """

        index = self._controllers.pop(controller_id, None)
        if index is not None:
            self._nxbt.remove_controller(index)

    def close(self) -> None:
        """Release every controller, then stop the NXBT manager process."""

        for controller_id in list(self._controllers):
            self.detach(controller_id)
        self._nxbt._on_exit()

    def _controller_index(self, controller_id: int) -> int:
        """Return the NXBT index for one active bridge slot.

        Args:
            controller_id: Bridge-owned controller slot.
        Returns:
            NXBT controller index.
        Raises:
            OSError: If the slot was never attached or has been detached.
        """

        try:
            return self._controllers[controller_id]
        except KeyError as error:
            raise OSError("controller slot is not attached") from error

    def _status(self, controller_id: int) -> ControllerStatus:
        """Map NXBT's controller-state string to the protocol status enum.

        Args:
            controller_id: Bridge-owned controller slot.
        Returns:
            Current protocol connection status.
        """

        state = self._nxbt.state[self._controller_index(controller_id)]["state"]
        return {
            "connected": ControllerStatus.CONNECTED,
            "connecting": ControllerStatus.CONNECTING,
            "reconnecting": ControllerStatus.RECONNECTING,
        }.get(state, ControllerStatus.FAILED)

    @staticmethod
    def _axis_percent(value: int) -> int:
        """Convert a signed Sunshine axis to NXBT's bounded direct-input percentage.

        Args:
            value: Sunshine signed 16-bit axis value.
        Returns:
            NXBT direct-input axis value in [-100, 100].
        """

        divisor = 32768 if value < 0 else 32767
        return max(-100, min(100, round(value * 100 / divisor)))


class Bridge:
    """Protocol state machine that owns controller slots and watchdog state."""

    def __init__(self, backend: Backend, clock: Clock, watchdog_us: int = DEFAULT_WATCHDOG_US) -> None:
        """Create an in-memory Bridge state machine.

        Args:
            backend: Bluetooth-facing controller backend.
            clock: Injectable monotonic clock.
            watchdog_us: Maximum input/heartbeat inactivity before neutralizing.
        """

        self._backend = backend
        self._clock = clock
        self._watchdog_us = watchdog_us
        self._controllers: dict[int, ControllerRecord] = {}
        self._hello_clients: set[object] = set()

    def handle_packet(self, client: object, packet: bytes) -> list[bytes]:
        """Handle one complete client packet and return immediate responses.

        Args:
            client: Stable identity for the connected Unix-socket client.
            packet: One complete protocol packet.
        Returns:
            Encoded protocol replies, including errors and status updates.
        """

        try:
            message = decode_message(packet)
        except ValueError as error:
            return [self._error_reply(error.args[0])]
        return self.handle_message(client, message)

    def handle_message(self, client: object, message: Message) -> list[bytes]:
        """Handle a decoded client message and return immediate replies.

        Args:
            client: Stable identity for the connected Unix-socket client.
            message: Valid decoded client message.
        Returns:
            Encoded protocol replies, including errors and status updates.
        """

        now_us = self._clock.now_us()
        if message.message_type == MessageType.HELLO:
            self._hello_clients.add(client)
            return [encode_message(Message(MessageType.HELLO_ACK, capabilities=message.capabilities))]
        if client not in self._hello_clients:
            return [self._error_reply(ProtocolError.UNSUPPORTED_VERSION)]
        if message.message_type == MessageType.ATTACH:
            return self._attach(client, message, now_us)
        if message.message_type == MessageType.REBIND:
            return self._rebind(client, message)
        if message.message_type == MessageType.STATE:
            return self._state(client, message.state, now_us)
        if message.message_type == MessageType.NEUTRALIZE:
            return self._neutralize(client, message.controller_id)
        if message.message_type == MessageType.DETACH:
            return self._detach(client, message.controller_id)
        if message.message_type == MessageType.PING:
            self._touch_owned(client, now_us)
            return [encode_message(Message(MessageType.PONG, monotonic_timestamp_us=message.monotonic_timestamp_us))]
        return [self._error_reply(ProtocolError.UNKNOWN_MESSAGE_TYPE)]

    def watchdog(self) -> None:
        """Neutralize each stale controller once using the injected monotonic clock."""

        now_us = self._clock.now_us()
        for controller_id, record in self._controllers.items():
            if not record.neutral and now_us - record.last_activity_us > self._watchdog_us:
                self._neutralize_record(controller_id, record)

    def flush_states(self) -> list[tuple[object, bytes]]:
        """Apply at most one newest snapshot per controller and report backend failures.

        Returns:
            Owner/reply pairs for backend failures discovered while flushing.
        """

        replies: list[tuple[object, bytes]] = []
        for controller_id, record in self._controllers.items():
            if not record.dirty:
                continue
            try:
                self._backend.apply_state(record.latest_state)
            except OSError:
                record.dirty = False
                record.neutral = True
                record.status = ControllerStatus.FAILED
                replies.append((record.owner, self._status_reply(controller_id, ControllerStatus.FAILED)))
                continue
            record.dirty = False
            record.neutral = False
        return replies

    def poll_statuses(self) -> list[tuple[object, bytes]]:
        """Report controller connection transitions exactly once per change.

        Returns:
            Owner/reply pairs for statuses that differ from the last report.
        """

        replies: list[tuple[object, bytes]] = []
        for controller_id, record in self._controllers.items():
            try:
                status = self._backend.status(controller_id)
            except OSError:
                status = ControllerStatus.FAILED
            if status != record.status:
                record.status = status
                replies.append((record.owner, self._status_reply(controller_id, status)))
        return replies

    def disconnect(self, client: object) -> None:
        """Immediately neutralize and detach every controller owned by a client.

        Args:
            client: Identity of the disconnected client.
        """

        for controller_id in [slot for slot, record in self._controllers.items() if record.owner is client]:
            record = self._controllers[controller_id]
            self._neutralize_record(controller_id, record)
            self._backend.detach(controller_id)
            del self._controllers[controller_id]
        self._hello_clients.discard(client)

    def shutdown(self) -> None:
        """Neutralize every controller before releasing backend resources."""

        for controller_id, record in list(self._controllers.items()):
            self._neutralize_record(controller_id, record)
            self._backend.detach(controller_id)
        self._controllers.clear()
        self._hello_clients.clear()
        self._backend.close()

    def _attach(self, client: object, message: Message, now_us: int) -> list[bytes]:
        """Allocate an unowned controller slot and report its backend status."""

        if message.controller_id in self._controllers:
            return [self._error_reply(ProtocolError.INVALID_LENGTH)]
        try:
            status = self._backend.attach(message.controller_id)
        except OSError:
            return [self._status_reply(message.controller_id, ControllerStatus.FAILED)]
        self._controllers[message.controller_id] = ControllerRecord(client, message.client_relative_id, last_activity_us=now_us, status=status)
        return [self._status_reply(message.controller_id, status)]

    def _rebind(self, client: object, message: Message) -> list[bytes]:
        """Update one owned slot's Moonlight client-relative id."""

        record = self._controllers.get(message.controller_id)
        if record is None or record.owner is not client:
            return [self._error_reply(ProtocolError.INVALID_LENGTH)]
        record.client_relative_id = message.client_relative_id
        return []

    def _state(self, client: object, state: ControllerState, now_us: int) -> list[bytes]:
        """Apply only a newer, non-regressing full state snapshot."""

        record = self._controllers.get(state.controller_id)
        if record is None or record.owner is not client:
            return [self._error_reply(ProtocolError.INVALID_LENGTH)]
        if record.accepted_state and (not sequence_is_newer(state.sequence, record.latest_state.sequence) or state.monotonic_timestamp_us < record.last_timestamp_us):
            return []
        record.latest_state = state
        record.last_timestamp_us = state.monotonic_timestamp_us
        record.last_activity_us = now_us
        record.accepted_state = True
        record.dirty = True
        return []

    def _neutralize(self, client: object, controller_id: int) -> list[bytes]:
        """Neutralize one owned slot while preserving its attachment."""

        record = self._controllers.get(controller_id)
        if record is None or record.owner is not client:
            return [self._error_reply(ProtocolError.INVALID_LENGTH)]
        self._neutralize_record(controller_id, record)
        return []

    def _detach(self, client: object, controller_id: int) -> list[bytes]:
        """Neutralize, release, and forget one owned slot."""

        record = self._controllers.get(controller_id)
        if record is None or record.owner is not client:
            return [self._error_reply(ProtocolError.INVALID_LENGTH)]
        self._neutralize_record(controller_id, record)
        self._backend.detach(controller_id)
        del self._controllers[controller_id]
        return []

    def _neutralize_record(self, controller_id: int, record: ControllerRecord) -> None:
        """Issue at most one backend neutralization until a new state arrives."""

        if not record.neutral:
            self._backend.neutralize(controller_id)
            record.neutral = True
        record.dirty = False

    def _touch_owned(self, client: object, now_us: int) -> None:
        """Refresh watchdog activity for every controller owned by a healthy client."""

        for record in self._controllers.values():
            if record.owner is client:
                record.last_activity_us = now_us

    @staticmethod
    def _error_reply(error: ProtocolError) -> bytes:
        """Encode an error response without exposing sensitive backend details."""

        return encode_message(Message(MessageType.ERROR, error=error))

    @staticmethod
    def _status_reply(controller_id: int, status: ControllerStatus) -> bytes:
        """Encode one proactive controller-status response."""

        return encode_message(Message(MessageType.STATUS, controller_id=controller_id, status=status))


class UnixBridgeServer:
    """Unix `SOCK_SEQPACKET` server that delegates packet handling to ``Bridge``."""

    def __init__(self, bridge: Bridge, socket_path: str = DEFAULT_SOCKET_PATH) -> None:
        """Create a server without binding the requested socket path.

        Args:
            bridge: State machine serving accepted client packets.
            socket_path: Local Unix-socket path to bind.
        """

        self._bridge = bridge
        self._socket_path = Path(socket_path)
        self._selector = selectors.DefaultSelector()
        self._listener: socket.socket | None = None
        self._stopped = threading.Event()
        self._shutdown = False

    def bind(self) -> None:
        """Bind a restricted-permission Unix packet socket, recovering a stale socket.

        Raises:
            RuntimeError: If the target path is not a stale Unix socket.
        """

        self._socket_path.parent.mkdir(mode=0o770, parents=True, exist_ok=True)
        if self._socket_path.exists():
            if not self._socket_path.is_socket():
                raise RuntimeError(f"refusing to replace non-socket path: {self._socket_path}")
            self._socket_path.unlink()
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        listener.bind(str(self._socket_path))
        os.chmod(self._socket_path, 0o660)
        listener.listen()
        listener.setblocking(False)
        self._listener = listener
        self._selector.register(listener, selectors.EVENT_READ, self._accept)

    def serve_forever(self) -> None:
        """Process IPC events and watchdog checks until ``stop`` is requested."""

        if self._listener is None:
            self.bind()
        while not self._stopped.is_set():
            for key, _ in self._selector.select(timeout=0.05):
                key.data(key.fileobj)
            for owner, reply in self._bridge.flush_states():
                if isinstance(owner, socket.socket):
                    owner.sendall(reply)
            for owner, reply in self._bridge.poll_statuses():
                if isinstance(owner, socket.socket):
                    owner.sendall(reply)
            self._bridge.watchdog()
        self.stop()

    def request_stop(self) -> None:
        """Request shutdown without closing descriptors from a signal handler."""

        if self._stopped.is_set():
            return
        self._stopped.set()
        self._bridge.shutdown()

    def stop(self) -> None:
        """Stop event processing and neutralize backend input before releasing the socket."""

        if self._shutdown:
            return
        self._shutdown = True
        self.request_stop()
        self._selector.close()
        if self._listener is not None:
            self._listener.close()
            self._listener = None
        if self._socket_path.exists() and self._socket_path.is_socket():
            self._socket_path.unlink()

    def _accept(self, listener: socket.socket) -> None:
        """Register one non-blocking client connection.

        Args:
            listener: Bound listening socket.
        """

        client, _ = listener.accept()
        client.setblocking(False)
        self._selector.register(client, selectors.EVENT_READ, self._read)

    def _read(self, client: socket.socket) -> None:
        """Receive one complete packet, send replies, or clean up a disconnect.

        Args:
            client: Connected Unix packet socket.
        """

        while True:
            try:
                packet = client.recv(4096)
            except BlockingIOError:
                return
            except OSError:
                packet = b""
            if not packet:
                self._selector.unregister(client)
                self._bridge.disconnect(client)
                client.close()
                return
            for reply in self._bridge.handle_packet(client, packet):
                client.sendall(reply)


def install_signal_handlers(server: UnixBridgeServer) -> None:
    """Arrange graceful SIGINT and SIGTERM shutdown for a foreground bridge server.

    Args:
        server: Server to stop after a termination signal.
    """

    def handle_signal(_signum: int, _frame: object) -> None:
        """Request bridge shutdown from a process signal handler."""

        server.request_stop()

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)
