"""Version 1 encoder and decoder for the local NXBT Bridge protocol."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum
import struct


MAGIC = b"NXBT"
VERSION = 1
HEADER_FORMAT = "<4sHBBI"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


class MessageType(IntEnum):
    """Protocol packet kinds."""

    HELLO = 1
    HELLO_ACK = 2
    ERROR = 3
    ATTACH = 4
    REBIND = 5
    STATE = 6
    NEUTRALIZE = 7
    DETACH = 8
    PING = 9
    PONG = 10
    STATUS = 11


class ProtocolError(IntEnum):
    """Protocol parsing and peer-reported error values."""

    NONE = 0
    BAD_MAGIC = 1
    UNSUPPORTED_VERSION = 2
    INVALID_LENGTH = 3
    UNKNOWN_MESSAGE_TYPE = 4
    TRUNCATED = 5


class ControllerStatus(IntEnum):
    """Bridge controller connection states."""

    UNAVAILABLE = 0
    PAIRING = 1
    CONNECTING = 2
    CONNECTED = 3
    RECONNECTING = 4
    FAILED = 5


@dataclass(frozen=True)
class ControllerState:
    """Complete input state for one Bridge-owned controller slot."""

    controller_id: int = 0
    button_flags: int = 0
    left_trigger: int = 0
    right_trigger: int = 0
    left_stick_x: int = 0
    left_stick_y: int = 0
    right_stick_x: int = 0
    right_stick_y: int = 0
    sequence: int = 0
    monotonic_timestamp_us: int = 0


@dataclass(frozen=True)
class Message:
    """Decoded or serializable NXBT Bridge message."""

    message_type: MessageType
    capabilities: int = 0
    controller_id: int = 0
    client_relative_id: int = 0
    state: ControllerState = field(default_factory=ControllerState)
    monotonic_timestamp_us: int = 0
    status: ControllerStatus = ControllerStatus.UNAVAILABLE
    error: ProtocolError = ProtocolError.NONE


def _payload_size(message_type: MessageType) -> int:
    """Return the protocol-defined fixed payload length for a message type."""

    if message_type in (MessageType.HELLO, MessageType.HELLO_ACK):
        return 4
    if message_type in (MessageType.ERROR, MessageType.ATTACH, MessageType.REBIND, MessageType.NEUTRALIZE, MessageType.DETACH, MessageType.STATUS):
        return 4
    if message_type == MessageType.STATE:
        return 28
    if message_type in (MessageType.PING, MessageType.PONG):
        return 8
    raise ValueError("unknown message type")


def encode_message(message: Message) -> bytes:
    """Encode one protocol message with explicit little-endian fields."""

    message_type = MessageType(message.message_type)
    if message_type in (MessageType.HELLO, MessageType.HELLO_ACK):
        payload = struct.pack("<I", message.capabilities)
    elif message_type == MessageType.ERROR:
        payload = struct.pack("<HH", message.error, 0)
    elif message_type in (MessageType.ATTACH, MessageType.REBIND):
        payload = struct.pack("<BBH", message.controller_id, message.client_relative_id, 0)
    elif message_type == MessageType.STATE:
        state = message.state
        payload = struct.pack(
            "<BBBBIhhhhIQ",
            state.controller_id,
            state.left_trigger,
            state.right_trigger,
            0,
            state.button_flags,
            state.left_stick_x,
            state.left_stick_y,
            state.right_stick_x,
            state.right_stick_y,
            state.sequence,
            state.monotonic_timestamp_us,
        )
    elif message_type in (MessageType.NEUTRALIZE, MessageType.DETACH):
        payload = struct.pack("<B3x", message.controller_id)
    elif message_type in (MessageType.PING, MessageType.PONG):
        payload = struct.pack("<Q", message.monotonic_timestamp_us)
    elif message_type == MessageType.STATUS:
        payload = struct.pack("<BBH", message.controller_id, message.status, 0)
    else:
        raise ValueError("unknown message type")
    return struct.pack(HEADER_FORMAT, MAGIC, VERSION, message_type, 0, len(payload)) + payload


def decode_message(packet: bytes) -> Message:
    """Decode one complete packet or raise a value-specific ``ValueError``."""

    if len(packet) < HEADER_SIZE:
        raise ValueError(ProtocolError.TRUNCATED)
    magic, version, raw_type, _reserved, length = struct.unpack_from(HEADER_FORMAT, packet)
    if magic != MAGIC:
        raise ValueError(ProtocolError.BAD_MAGIC)
    if version != VERSION:
        raise ValueError(ProtocolError.UNSUPPORTED_VERSION)
    try:
        message_type = MessageType(raw_type)
    except ValueError as error:
        raise ValueError(ProtocolError.UNKNOWN_MESSAGE_TYPE) from error
    if length != _payload_size(message_type):
        raise ValueError(ProtocolError.INVALID_LENGTH)
    if len(packet) < HEADER_SIZE + length:
        raise ValueError(ProtocolError.TRUNCATED)
    if len(packet) != HEADER_SIZE + length:
        raise ValueError(ProtocolError.INVALID_LENGTH)
    payload = memoryview(packet)[HEADER_SIZE:]
    if message_type in (MessageType.HELLO, MessageType.HELLO_ACK):
        return Message(message_type, capabilities=struct.unpack("<I", payload)[0])
    if message_type == MessageType.ERROR:
        return Message(message_type, error=ProtocolError(struct.unpack_from("<H", payload)[0]))
    if message_type in (MessageType.ATTACH, MessageType.REBIND):
        controller_id, client_relative_id = struct.unpack("<BB", payload[:2])
        return Message(message_type, controller_id=controller_id, client_relative_id=client_relative_id)
    if message_type == MessageType.STATE:
        fields = struct.unpack("<BBBBIhhhhIQ", payload)
        return Message(message_type, state=ControllerState(fields[0], fields[4], fields[1], fields[2], *fields[5:]))
    if message_type in (MessageType.NEUTRALIZE, MessageType.DETACH):
        return Message(message_type, controller_id=payload[0])
    if message_type in (MessageType.PING, MessageType.PONG):
        return Message(message_type, monotonic_timestamp_us=struct.unpack_from("<Q", payload)[0])
    controller_id, status = struct.unpack("<BB", payload[:2])
    return Message(message_type, controller_id=controller_id, status=ControllerStatus(status))


def sequence_is_newer(candidate: int, current: int) -> bool:
    """Return whether an unsigned 32-bit sequence is newer after wrapping."""

    candidate &= 0xFFFFFFFF
    current &= 0xFFFFFFFF
    return candidate != current and ((candidate - current) & 0xFFFFFFFF) < 0x80000000
