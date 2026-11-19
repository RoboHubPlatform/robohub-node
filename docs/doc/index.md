# Transport

Framing for a byte stream. A UART, a USB CDC pipe, an RS-485 bus and a socket all
deliver a message as bytes with no idea where it ends; this turns that stream
back into messages and hands each one to the runtime's event bus.

```
robohub-core        event bus / rpc dispatch
      ▲  validated frame
robohub-transport   COBS · Fletcher-16 · parser · counters
      ▲  raw bytes
physical driver     Pico USB CDC / ESP32 UART / STM32 SPI / socket
```

## A frame

```
msg_id      u16   which event the payload is
tx_id       u16   0 for telemetry, shared by a call and its answer
payload_len u16   N
payload     [N]   the packed bytes a schema generated
check       u16   Fletcher-16 over the header and the payload
```

Little-endian, then COBS-encoded, then terminated by `0x00`. COBS guarantees that
byte appears nowhere else, so the parser finds a boundary the instant it sees
one — even joining a stream half way through a frame, or after a burst of noise.
A protocol that resynchronises on a timeout is a protocol that stalls when the
timer is wrong.

## No allocation

The receive buffer is in the instance and a complete frame is decoded onto the
stack. `robohub_transport_feed_byte` never blocks and never allocates, so it is
safe to call from the interrupt or DMA callback that delivered the byte.

```c
RoboHubTransport link;
robohub_transport_init(&link, &(RoboHubTransportPhy){ .write = uart_write, .context = &uart });

/* In the UART interrupt. */
robohub_transport_feed_byte(&link, received_byte);

/* To send. */
robohub_transport_send(&link, EVENT_TEMP_READING, 0, &reading, sizeof(reading));
```

A frame that checks out is published with `robohub_core_event_publish_with_tx`,
so a handler is reached by the event its `msg_id` names, and a request and its
response can share a `tx_id`.

## Counters

`check_errors` (frames whose Fletcher-16 check did not match), `rx_packets`,
`tx_packets` and `dropped` are on the instance. A link that is dropping
everything says so, which on a board with no debugger is the difference between
a wire that is wrong and a program that is.

The check is Fletcher-16, not a CRC. Releases before 0.4.2 named the counter
`crc_errors` and the check's size `RH_TRANSPORT_CRC_SIZE`; both names are kept
as deprecated aliases of `check_errors` and `RH_TRANSPORT_CHECK_SIZE` (the
counter is the same field, the size the same value) so existing code compiles,
and will be removed in a later minor release.

## Deployment transports

The frames are the same over every byte stream. The module also declares the
transports it can carry them over, in `capabilities.transports`:

```yaml
capabilities:
  transports: "uart console usb wifi ethernet socket rs485 spi i2c"
```

A board declares what it has in `capabilities.links` and `capabilities.buses`.
A deployment planner intersects the module's list with both ends of a
connection, so it offers only transports both boards can serve. `console` is a
host's byte stream and is offered only when the parent is a host. Adding a
transport to the module, or a link to a board, makes it selectable without any
planner change.

## Building

It is a native module: `rho build` adds it as a dependency, and it builds on its
own with CMake for the host tests.

## Authenticated frames

`robohub/secure.h` authenticates a link: `robohub_transport_secure(transport,
&secure)` and from then on every frame sent is sealed and every frame received
must open under the session, or it is dropped. The first send says hello to the
gateway and returns `ROBOHUB_ERR_STATE` until the session is up, so nothing is
sent unauthenticated. HMAC-SHA256, truncated to 64 bits, with a counter per
direction and a session lifetime; no encryption, no allocation. The board's id and
device key come from its firmware build, and its nonces from
`robohub_platform_nonce`, which every platform provides.

### Key epochs and rotation

A board's key can be rotated by its gateway without reflashing it. The firmware
is built with its key and that key's epoch (`RH_DEVICE_KEY_EPOCH` in the
`robohub_secret.h` Mesh writes) and binds them with
`robohub_secure_init_epoch` (with its scope); `robohub_secure_init` is epoch 0
with no scope. At init the
platform's keystore (`robohub/keystore.h`, in robohub-common) is read, and a key
it holds of a newer epoch is used instead — when it was stored under the build's
key scope (`RH_KEY_SCOPE` in `robohub_secret.h`, one per gateway). A key stored
for another gateway, by a board since moved to another mesh, is ignored. A build
without a scope (a Linux host node, whose key is in a file) passes none, has the
scope of eight zero bytes, and takes only keys stored that way.

When the gateway sends `REKEY` (`0x0014`) sealed under the session, the
transport hands it to `robohub_secure_rekey` and nothing above the transport
sees it: the new key is unwrapped (it is XORed with an HMAC of the current key,
the frame's session id and its counter), stored with `rh_keystore_store` — or
with the instance's own store, `robohub_secure_on_store`, which a Linux host
node uses to rewrite its key file — and answered with `REKEY_ACK` (`0x0015`),
sealed under the same session, carrying a proof made with the new key. The
session goes on under the old key; the next hello is under the new one. A board
that cannot store the key answers epoch 0 and a zero proof, and keeps its key.
The derivation, the wrap and the proof are written out in `secure.h`.

Whoever holds a board's old key and recorded the `REKEY` frame can unwrap the new
key; there is no forward secrecy.

When the gateway sends `SESSION_RESET` (`0x0016`) and it opens under the current
session — its `K_down`, a counter greater than the last accepted — the transport
drops the session (`robohub_secure_reset`) and says a hello at once; nothing above
the transport sees it. A gateway sends it after a restart, for a session it took
back from disk: it can only place that session's upward counter past a margin,
and a board still counting below it would go unheard until it passed it.
Unsealed, or under any other session, the frame is refused like any other.

### Known-answer vectors

The board and the gateway implement the secure link apart, in C and in Rho. One
file holds the answers both are checked against: `tests/vectors/secure.json`,
here, is the canonical copy. It is written once by `tests/vectors/generate.py`,
an independent transcription of `secure.h` with nothing random in it — the key
derivations of epochs 0, 1 and 2, the hellos under epochs 0 and 1, a session
and its two keys, sealed frames both ways with their counters (a reading, a
command and its acknowledgement), `SESSION_RESET`, `REKEY` with its wrap and
`REKEY_ACK` with its proof — each as its payload and as the whole frame on the
wire.

- `tests/test_vectors.c` checks the board byte for byte, through
  `robohub_secure_*` and through the transport.
- robohub-gateway keeps an identical copy in its `tests/vectors/secure.json`
  and checks the gateway binary against it (`tools/vectors-smoke.py`); the first
  thing that suite checks is that its copy is this file, when a robohub-transport
  checkout is beside it (or under `ROBOHUB_WORKSPACE`).
- The `vectors` suite runs `generate.py --check`, so the file is what the
  generator writes.

A change to the protocol is a change to `generate.py`: run it, copy the file to
robohub-gateway, and both suites say whether their side follows. A vector one
side does not meet is a protocol question, not a test to update.

## Taking a frame before the bus

`robohub_transport_on_frame(&link, hook, user)` hands every frame that checks out
— on an authenticated link, after it opened — to `hook` before it is published;
a frame the hook takes (returns non-zero for) is not published. A relay sets one
on the link its boards are on: what it does with a board's frame is carry it
whole — its id, sealed or not, its transaction and its payload — inside a
`RELAYED` message to its parent, rather than act on it.

## Network transports

The capability list includes `http` and `mqtt`. A board whose platform supplies
an HTTP client or an MQTT client can select them; the framing and Fletcher-16 check stay
the same, and the device integration names the topic or the RPC path.
