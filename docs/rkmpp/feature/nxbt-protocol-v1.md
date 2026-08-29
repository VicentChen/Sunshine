# NXBT Bridge protocol version 1

This document freezes the local Unix-domain-socket protocol used between
Sunshine and `nxbt-bridge`. All integers use little-endian byte order. The
protocol never serializes a C++ or Python structure directly.

## Packet framing

Every `SOCK_SEQPACKET` packet is exactly one message. A future `SOCK_STREAM`
transport must retain this framing by reading the fixed 12-byte header, then
exactly the advertised payload length.

| Offset | Size | Field | Value |
| --- | ---: | --- | --- |
| 0 | 4 | magic | ASCII `NXBT` (`4e 58 42 54`) |
| 4 | 2 | version | `1` |
| 6 | 1 | message type | Values below |
| 7 | 1 | reserved | Sender writes zero; receiver ignores it |
| 8 | 4 | payload length | Exact fixed payload length for the type |

Packets with a different magic, unsupported version, unknown type, an
incorrect fixed length, trailing bytes, or truncation are rejected. Protocol
error values are `bad_magic=1`, `unsupported_version=2`, `invalid_length=3`,
`unknown_message_type=4`, and `truncated=5`.

## Messages

| Type | Name | Payload |
| ---: | --- | --- |
| 1 | `hello` | `uint32 capabilities` |
| 2 | `hello_ack` | `uint32 capabilities` |
| 3 | `error` | `uint16 error`, `uint16 reserved=0` |
| 4 | `attach` | `uint8 controller_id`, `uint8 client_relative_id`, `uint16 reserved=0` |
| 5 | `rebind` | Same as `attach` |
| 6 | `state` | Complete 28-byte state described below |
| 7 | `neutralize` | `uint8 controller_id`, three reserved zero bytes |
| 8 | `detach` | `uint8 controller_id`, three reserved zero bytes |
| 9 | `ping` | `uint64 monotonic_timestamp_us` |
| 10 | `pong` | `uint64 monotonic_timestamp_us` |
| 11 | `status` | `uint8 controller_id`, `uint8 status`, `uint16 reserved=0` |

`controller_id` is a Bridge-owned logical slot. `client_relative_id` is the
Moonlight controller number used only for feedback routing. A sender must
process `attach`, `rebind`, `neutralize`, and `detach` in order. A Bridge may
discard stale queued `state` messages only after retaining the newest state
for that controller.

## State payload

| Offset | Size | Field | Meaning |
| --- | ---: | --- | --- |
| 0 | 1 | controller id | Bridge controller slot |
| 1 | 1 | left trigger | Original Sunshine `lt`, 0–255 |
| 2 | 1 | right trigger | Original Sunshine `rt`, 0–255 |
| 3 | 1 | reserved | Zero |
| 4 | 4 | button flags | NXBT Bridge button bit mask |
| 8 | 2 | left stick X | `int16`, right is positive |
| 10 | 2 | left stick Y | `int16`, up is positive |
| 12 | 2 | right stick X | `int16`, right is positive |
| 14 | 2 | right stick Y | `int16`, up is positive |
| 16 | 4 | sequence | Per-controller `uint32` |
| 20 | 8 | timestamp | Sender monotonic microseconds (`uint64`) |

Sequence numbers are strictly newer when `(candidate - current) mod 2^32` is
between 1 and `0x7fffffff`. Equal values and the exact half-range difference
are not newer. The Bridge rejects a non-newer state and an implementation may
also reject a state that violates its monotonic-clock freshness limit.

The NXBT Bridge button bits are: D-pad up/down/left/right (`0–3`), PLUS/MINUS
(`4–5`), left/right stick press (`6–7`), L/R (`8–9`), HOME/CAPTURE (`10–11`),
Y/X/B/A (`12–15`), and ZL/ZR (`16–17`).

Status values are `unavailable=0`, `pairing=1`, `connecting=2`,
`connected=3`, `reconnecting=4`, and `failed=5`.

## Golden vectors

The C++ and Python test suites use matching vectors. These representative
vectors are listed as lowercase hexadecimal packet bytes:

| Message | Vector |
| --- | --- |
| Neutral state | `4e584254010006001c0000000000000000000000000000000000000000000000000000000000000000` |
| Attach slot 7/client 2 | `4e584254010004000400000007020000` |
| Full state | `4e584254010006001c00000007ff8000ffff03000080ff7fffff0100feffffff0807060504030201` |
| Neutralize slot 7 | `4e584254010007000400000007000000` |
| Detach slot 7 | `4e584254010008000400000007000000` |
| Connected status for slot 7 | `4e58425401000b000400000007030000` |
