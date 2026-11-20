# Changelog

All notable changes to this module are recorded here, one section per release.

## [0.2.1] - 2026-11-02

Network transports `http` and `mqtt` are listed in the capabilities, so a deployment can select a device path the platform supplies.

## [0.2.0] - 2026-11-01

`capabilities.transports` names the byte streams this runtime can carry frames
over — UART, Console, USB, WiFi, Ethernet, Socket, RS-485, SPI and I2C. A
deployment planner intersects it with both boards' `capabilities.links`.

## [0.3.0] - 2026-11-14

`robohub_transport_on_frame` hands every frame that checks out to a hook before the event bus, and a frame the hook takes is not published: what a relay needs to forward sealed frames it cannot open.

## [0.4.0] - 2026-11-14

Keys have epochs: `robohub_secure_init_epoch` takes the build's epoch and key scope, a `REKEY` from the gateway is unwrapped, stored in the platform keystore with the scope and confirmed, and the new key is used from the next hello. A stored key from another scope is ignored.

## [0.4.1] - 2026-11-14

A sealed `SESSION_RESET` under the current session drops it and sends a new HELLO at once.

## [0.4.2] - 2026-11-19

Known-answer vectors for the secure link, shared with robohub-gateway: `tests/vectors/secure.json`, written once by `tests/vectors/generate.py` (an independent transcription of `secure.h`) — the key derivations, the hellos under epochs 0 and 1, a session and its keys, sealed frames both ways with their counters, `SESSION_RESET`, `REKEY` and the `REKEY_ACK` proof. `tests/test_vectors.c` checks the board against it byte for byte, through the secure layer and on the wire; the gateway checks its side against the same file. Before, the board was tested only against a gateway re-implemented in C inside its own test, so both sides could have drifted the same way unnoticed. The secure layer is unchanged.

The frame check is Fletcher-16, and the header's `@brief`, the documentation, the counter and a constant called it a CRC. The correctly named `check_errors` counter and `RH_TRANSPORT_CHECK_SIZE` are added and the module uses them; `crc_errors` (the same field, through an anonymous union, marked deprecated for GCC and Clang) and `RH_TRANSPORT_CRC_SIZE` (the same value) stay as deprecated aliases, so existing C code still compiles, and will be removed in a later minor release. The documentation calls the frame's last field `check`.

## [0.4.3] - 2026-11-20

The build finds the modules it needs inside the robohub-node collection (robohub-core in `core/`, robohub-transport in `link/`, robohub-telemetry in `telemetry/`), and fetches robohub-node, never the archived repositories, when it is built outside a workspace. It found them only as sibling repositories, so once those were archived a build fetched their last state from GitHub.
