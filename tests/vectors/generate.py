#!/usr/bin/env python3
#
# @file generate.py
# @brief Writes secure.json: known-answer vectors for robohub-transport's sealed frames.
#
# @project RoboHub Platform
# @author Aurel Miron
# @date 2026-11-19
#
# SPDX-License-Identifier: MIT
#
"""Known-answer vectors for the secure link, from the protocol in secure.h.

This is the reference the two implementations are held to: the board's
(src/secure.c, through tests/test_vectors.c) and the gateway's
(robohub-gateway's secure.rho, through its tools/vectors-smoke.py). It is an
independent, plain transcription of include/robohub/secure.h — HMAC-SHA256 from
Python's standard library, no code shared with either side — run once; the
JSON it writes is checked in, and both test suites compare what their side
computes against it byte for byte.

Nothing here is random. Every input is fixed below, so running it again writes
the same file; a change to the file is a change to the protocol.

    python3 tests/vectors/generate.py        # rewrites tests/vectors/secure.json
    python3 tests/vectors/generate.py --check
"""
import hashlib
import hmac
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "secure.json")

FLAG = 0x8000
MSG_HELLO = 0x0011
MSG_SESSION = 0x0012
MSG_REKEY = 0x0014
MSG_REKEY_ACK = 0x0015
MSG_SESSION_RESET = 0x0016
MSG_TELEMETRY = 0x0201
MSG_CONTROL_REQUEST = 0x0301
MSG_CONTROL_RESPONSE = 0x0302


def mac(key: bytes, *parts: bytes) -> bytes:
    return hmac.new(key, b"".join(parts), hashlib.sha256).digest()


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def fletcher16(data: bytes) -> int:
    s1 = s2 = 0
    for b in data:
        s1 = (s1 + b) % 255
        s2 = (s2 + s1) % 255
    return (s2 << 8) | s1


def cobs(data: bytes) -> bytes:
    out = bytearray([0])
    code_at, code = 0, 1
    for b in data:
        if b == 0:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                code_at, code = len(out), 1
                out.append(0)
    out[code_at] = code
    return bytes(out)


def frame(msg_id: int, tx_id: int, payload: bytes) -> bytes:
    """A frame on the wire: COBS over header, payload and Fletcher-16, then 0x00."""
    body = u16(msg_id) + u16(tx_id) + u16(len(payload)) + payload
    return cobs(body + u16(fletcher16(body))) + b"\x00"


def device(area: int, node: int) -> bytes:
    return b"\x01" + u16(area) + u16(node)


def k_dev(k_gw: bytes, uid: bytes, epoch: int) -> bytes:
    if epoch == 0:
        return mac(k_gw, b"rh-dev1", uid)[:16]
    return mac(k_gw, b"rh-dev2", uid, u32(epoch))[:16]


def seal(key: bytes, sid: bytes, counter: int, msg_id: int, tx_id: int, inner: bytes) -> bytes:
    flagged = msg_id | FLAG
    sid_counter = sid + u32(counter)
    return sid_counter + inner + mac(key, u16(flagged), u16(tx_id), sid_counter, inner)[:8]


def sealed_vector(key, sid, counter, msg_id, tx_id, inner, **extra):
    payload = seal(key, sid, counter, msg_id, tx_id, inner)
    vector = {
        "msg_id": msg_id,
        "wire_msg_id": msg_id | FLAG,
        "tx_id": tx_id,
        "counter": counter,
        "inner": inner.hex(),
        "payload": payload.hex(),
        "frame": frame(msg_id | FLAG, tx_id, payload).hex(),
    }
    vector.update(extra)
    return vector


def vectors() -> dict:
    k_gw = bytes(range(0x40, 0x60))
    uid = bytes([0xE6, 0x61, 0x41, 0x04, 0x03, 0x62, 0x6A, 0x2B])
    k0, k1 = k_dev(k_gw, uid, 0), k_dev(k_gw, uid, 1)
    area, node = 7, 11

    # HELLO under epoch 0, and another under epoch 1 (a board that holds the
    # rotated key says its next hello with it).
    nonce_b = bytes([0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7])
    hello = uid + nonce_b + mac(k0, b"rh-hello1", uid, nonce_b)[:8]
    nonce_b1 = bytes([0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7])
    hello1 = uid + nonce_b1 + mac(k1, b"rh-hello1", uid, nonce_b1)[:8]

    # The gateway's SESSION answering the epoch-0 hello.
    sid_value = 0x1234
    sid = u16(sid_value)
    nonce_g = bytes([0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02])
    lifetime = 3600
    session = uid + sid + nonce_g + u16(lifetime) + \
        mac(k0, b"rh-session1", uid, nonce_b, sid, nonce_g, u16(lifetime))[:8]
    k_up = mac(k0, b"rh-up1", nonce_b, nonce_g)[:16]
    k_down = mac(k0, b"rh-down1", nonce_b, nonce_g)[:16]

    # The session's counters. The gateway's test restores this session from a
    # sessions file holding these marks, and a restored session starts each
    # direction 2 x 64 past its mark (robohub-gateway, secure_restore): its
    # downward frames are then numbered 229, 230, 231 in the order below, and
    # an upward frame is taken from 229 on.
    stored_up, stored_down = 100, 100
    down0 = stored_down + 128

    reading = device(area, node) + u32(1000) + struct.pack("<f", 21.5) + u32(0)
    reading = b"\x01" + reading
    control = b"\x01" + device(area, node) + u16(1) + u32(250)
    control_ack = b"\x01" + device(area, node) + u16(1) + u16(0) + u32(240)

    # REKEY to epoch 1: the new key wrapped under the current one, bound to the
    # REKEY frame's own session id and counter.
    rekey_counter = down0 + 2
    mask = mac(k0, b"rh-wrap1", sid, u32(rekey_counter))[:16]
    rekey_inner = u32(1) + bytes(a ^ b for a, b in zip(k1, mask))
    proof = mac(k1, b"rh-rekey-ok", uid, u32(1))[:8]
    ack_inner = u32(1) + proof

    return {
        "about": ("Known-answer vectors for robohub-transport's sealed frames (include/robohub/secure.h). "
                  "Written once by tests/vectors/generate.py, an independent transcription of the protocol; "
                  "the board (tests/test_vectors.c) and robohub-gateway (tools/vectors-smoke.py) are each "
                  "checked against it byte for byte. All byte strings are lowercase hex; a frame is the "
                  "whole encoded frame, COBS and the closing 0x00 included."),
        "version": 1,
        "keys": {
            "k_gw": k_gw.hex(),
            "uid": uid.hex(),
            "k_dev_epoch0": k0.hex(),
            "k_dev_epoch1": k1.hex(),
            "k_dev_epoch2": k_dev(k_gw, uid, 2).hex(),
        },
        "placement": {"area": area, "node": node},
        "hello": {
            "epoch": 0,
            "nonce_b": nonce_b.hex(),
            "payload": hello.hex(),
            "frame": frame(MSG_HELLO, 0, hello).hex(),
        },
        "hello_epoch1": {
            "epoch": 1,
            "nonce_b": nonce_b1.hex(),
            "payload": hello1.hex(),
            "frame": frame(MSG_HELLO, 0, hello1).hex(),
        },
        "session": {
            "sid": sid_value,
            "sid_bytes": sid.hex(),
            "nonce_g": nonce_g.hex(),
            "lifetime_s": lifetime,
            "payload": session.hex(),
            "frame": frame(MSG_SESSION, 0, session).hex(),
            "k_up": k_up.hex(),
            "k_down": k_down.hex(),
            "stored_up": stored_up,
            "stored_down": stored_down,
        },
        # Gateway to board, in the order a restored gateway sends them.
        "session_reset": sealed_vector(k_down, sid, down0 + 1, MSG_SESSION_RESET, 0, b"\x01"),
        "rekey": sealed_vector(k_down, sid, rekey_counter, MSG_REKEY, 0, rekey_inner,
                               epoch=1, mask=mask.hex()),
        "control": sealed_vector(k_down, sid, down0 + 3, MSG_CONTROL_REQUEST, 1, control),
        # Board to gateway.
        "reading": sealed_vector(k_up, sid, 300, MSG_TELEMETRY, 0, reading, temperature_c=21.5),
        "rekey_ack": sealed_vector(k_up, sid, 301, MSG_REKEY_ACK, 0, ack_inner, epoch=1, proof=proof.hex()),
        "control_ack": sealed_vector(k_up, sid, 302, MSG_CONTROL_RESPONSE, 1, control_ack, actual=240),
    }


def render() -> str:
    return json.dumps(vectors(), indent=2) + "\n"


def main() -> int:
    text = render()
    if "--check" in sys.argv[1:]:
        with open(OUT) as f:
            if f.read() != text:
                print("secure.json is not what generate.py writes", file=sys.stderr)
                return 1
        print("secure.json matches generate.py")
        return 0
    with open(OUT, "w") as f:
        f.write(text)
    print("wrote", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
