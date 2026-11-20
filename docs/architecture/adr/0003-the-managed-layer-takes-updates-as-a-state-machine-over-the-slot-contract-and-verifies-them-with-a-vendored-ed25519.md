---
id: ADR-0003
title: The managed layer takes updates as a state machine over the slot contract, and verifies them with a vendored Ed25519
status: accepted
date: 2026-11-20
requirements: [RNODE-0012, RNODE-UPD-0001, RNODE-UPD-0002, RNODE-UPD-0003, RNODE-UPD-0004, RNODE-UPD-0005, RNODE-UPD-0006, RNODE-UPD-0007]
supersedes: []
superseded_by: []
---

# The managed layer takes updates as a state machine over the slot contract, and verifies them with a vendored Ed25519

## Context

robohub ADR-0022 fixes the protocol: an Ed25519-signed manifest, a window of four
chunks, verify then boot once, confirm after the session and 30 s healthy.
RNODE-0012 puts it in the managed layer, the top of the stack (RNODE-0003): above
the core, the link and telemetry, chosen in the manifest like the others.

## Decision

1. **A module of its own, `managed/`** (robohub-managed, prefix RNODE-UPD), which
   depends on the core, the link and robohub-common, and is compiled only when an
   application chooses it (`CONFIG_RH_NODE_MANAGED`). A board that does not choose
   it answers an offer with nothing — the gateway times out and Mesh says the
   board takes flashes only.
2. **One state machine**, idle → receiving → verified → (reboot) → pending →
   confirmed or rolled back, driven by the link's frames and the core's loop; one
   update at a time, its state in RAM. A reset in `receiving` drops it
   (ADR-0022, 4).
3. **Ed25519 vendored, SHA-256 shared.** The verifier is Monocypher's (CC0 or
   BSD-2, constant time, a few KB), vendored with its licence under
   `managed/vendor/`; the SHA-256 is the link's, which sealing already carries.
   The public key comes from a header the build generates beside
   `robohub_secret.h` (`robohub_update_key.h`); without it, the layer refuses
   every offer as not provisioned.
4. **Confirmation reuses 2a.** "Healthy for 30 s" is the core's own: the session
   is up and the watchdog has been fed continuously for 30 s — no new
   bookkeeping, and an image whose task hangs is never confirmed.
5. **Tested on a host.** The state machine is tested with a fake slot and a fake
   clock; the whole path with robohub-linux's file slot and a Python gateway
   that offers, sends, drops frames and cuts the link.

## Consequences

- The managed layer is the first to vendor third-party code; its licence goes in
  the collection's notices.
- A Rho binding for it waits for RNODE-0010's bindings.
- 2c and 2d add only platform slots; nothing in this module changes for them.

## Alternatives considered

- **The verifier in robohub-common** — every board would carry it, even ones that
  are never updated. Rejected: it belongs to the layer that needs it.
- **TweetNaCl** — smaller source, much slower verification. Rejected.
- **Writing Ed25519** — rejected outright.
