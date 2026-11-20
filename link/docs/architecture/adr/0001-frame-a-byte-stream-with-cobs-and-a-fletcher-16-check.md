---
id: ADR-0001
title: Frame a byte stream with COBS and a Fletcher-16 check
status: proposed
date: 2026-11-19
requirements: [TRAN-0001, TRAN-FRAME-0001]
supersedes: []
superseded_by: []
---

# Frame a byte stream with COBS and a Fletcher-16 check

## Context

A board's messages travel over a UART, a USB CDC pipe, an RS-485 bus or a
socket. Each delivers bytes with no message boundaries and some of them wrong,
and the parser must run from an interrupt on an 8-bit part with no heap. A
protocol that finds frames by a start pattern or by a silence on the line
stalls or splits frames when the pattern appears in a payload or the timer is
wrong.

## Decision

- Every frame is msg_id, tx_id, payload_len (little-endian u16), the payload and
  a 16-bit check, COBS-encoded and ended by a single 0x00. COBS guarantees the
  delimiter appears nowhere else, so the parser finds a boundary the instant it
  sees one, also when it joins a stream mid-frame or after noise.
- The check is Fletcher-16 over the header and payload: an add and a modulo per
  byte, with no table to carry, and it catches the flipped bit, dropped byte and
  transposition a serial line makes.
- The receive buffer is part of the transport instance
  (RH_TRANSPORT_MAX_FRAME_SIZE, 256 bytes by default, set in Kconfig), a frame is
  decoded onto the stack, and nothing allocates or blocks.
- Integrity only: whether a frame is genuine is the secure layer's question
  (robohub:ADR-0010), answered inside the payload.

## Consequences

- One parser serves every link, and the gateway reads the same frames from all
  of them; the telemetry bridge sends the same header and check over sockets
  without the COBS layer.
- Fletcher-16 is weaker than a CRC-16 against some burst errors; a frame it lets
  through is still refused by the secure layer's tag on an authenticated link.
- A frame longer than the configured maximum cannot be carried; a board that
  needs larger payloads raises the Kconfig value, at that cost per channel.
- The names `crc_errors` and `RH_TRANSPORT_CRC_SIZE` predate the choice and mean
  the Fletcher-16 check; since 0.4.2 they are deprecated aliases of
  `check_errors` and `RH_TRANSPORT_CHECK_SIZE`, and the documentation calls the
  frame's last field `check`.
