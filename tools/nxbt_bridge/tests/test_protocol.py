"""Tests for the NXBT Bridge version 1 protocol codec."""

import unittest

from tools.nxbt_bridge.protocol import ControllerState, ControllerStatus, Message, MessageType, ProtocolError, decode_message, encode_message, sequence_is_newer


class ProtocolTest(unittest.TestCase):
    """Verify the protocol codec and stable cross-language golden vectors."""

    def test_golden_vectors_round_trip(self):
        """Encode every version 1 packet kind to its fixed golden vector."""

        state = ControllerState(7, 0x0003FFFF, 255, 128, -32768, 32767, -1, 1, 0xFFFFFFFE, 0x0102030405060708)
        vectors = {
            Message(MessageType.HELLO, capabilities=0x11223344): "4e584254010001000400000044332211",
            Message(MessageType.HELLO_ACK, capabilities=0): "4e584254010002000400000000000000",
            Message(MessageType.ERROR, error=ProtocolError.INVALID_LENGTH): "4e584254010003000400000003000000",
            Message(MessageType.ATTACH, controller_id=7, client_relative_id=2): "4e584254010004000400000007020000",
            Message(MessageType.REBIND, controller_id=7, client_relative_id=3): "4e584254010005000400000007030000",
            Message(MessageType.STATE, state=state): "4e584254010006001c00000007ff8000ffff03000080ff7fffff0100feffffff0807060504030201",
            Message(MessageType.NEUTRALIZE, controller_id=7): "4e584254010007000400000007000000",
            Message(MessageType.DETACH, controller_id=7): "4e584254010008000400000007000000",
            Message(MessageType.PING, monotonic_timestamp_us=0x0102030405060708): "4e58425401000900080000000807060504030201",
            Message(MessageType.PONG, monotonic_timestamp_us=0x0102030405060708): "4e58425401000a00080000000807060504030201",
            Message(MessageType.STATUS, controller_id=7, status=ControllerStatus.CONNECTED): "4e58425401000b000400000007030000",
        }
        for message, expected in vectors.items():
            with self.subTest(message_type=message.message_type):
                encoded = encode_message(message)
                self.assertEqual(encoded.hex(), expected)
                self.assertEqual(decode_message(encoded), message)

    def test_rejects_invalid_packets(self):
        """Reject bad magic, version, type, length, and truncation distinctly."""

        packet = bytearray(encode_message(Message(MessageType.HELLO)))
        cases = [
            (b"bad", ProtocolError.TRUNCATED),
            (bytes(b"FAIL" + packet[4:]), ProtocolError.BAD_MAGIC),
            (bytes(packet[:4] + b"\x02\x00" + packet[6:]), ProtocolError.UNSUPPORTED_VERSION),
            (bytes(packet[:6] + b"\xff" + packet[7:]), ProtocolError.UNKNOWN_MESSAGE_TYPE),
            (bytes(packet[:8] + b"\x05\x00\x00\x00" + packet[12:]), ProtocolError.INVALID_LENGTH),
            (bytes(packet[:-1]), ProtocolError.TRUNCATED),
            (bytes(packet + b"\x00"), ProtocolError.INVALID_LENGTH),
        ]
        for malformed, expected in cases:
            with self.subTest(expected=expected):
                with self.assertRaisesRegex(ValueError, str(int(expected))):
                    decode_message(malformed)

    def test_sequence_wraparound(self):
        """Accept strict forward sequence movement across uint32 wrap-around."""

        self.assertTrue(sequence_is_newer(0, 0xFFFFFFFF))
        self.assertTrue(sequence_is_newer(1, 0xFFFFFFFE))
        self.assertFalse(sequence_is_newer(0xFFFFFFFF, 0))
        self.assertFalse(sequence_is_newer(5, 5))
        self.assertFalse(sequence_is_newer(0x80000000, 0))


if __name__ == "__main__":
    unittest.main()
