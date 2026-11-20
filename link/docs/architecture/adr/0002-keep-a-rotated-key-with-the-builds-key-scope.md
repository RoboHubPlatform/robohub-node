---
id: ADR-0002
title: Keep a rotated key with the build's key scope
status: proposed
date: 2026-11-19
requirements: [TRAN-0004, TRAN-SEC-0010]
supersedes: []
superseded_by: []
---

# Keep a rotated key with the build's key scope

## Context

A rotated key (REKEY, robohub:ADR-0010) is written to the board's keystore and
outlives a reboot, while the firmware also carries a compiled-in key and its
epoch. Two cases make a stored key wrong to use: the board was flashed again
after the rotation with a newer key, and the board was moved to another mesh
and flashed for that gateway, whose epochs are numbered independently and may
be lower than the stored one.

## Decision

- A stored key is used instead of the compiled-in one only when its epoch is
  greater than the compiled-in epoch (RH_DEVICE_KEY_EPOCH).
- Every stored key carries the build's key scope, RH_KEY_SCOPE from the
  robohub_secret.h Mesh writes: HMAC-SHA256(K_gw, "rh-scope1")[0:8], the same for
  every board of one gateway. A stored key is used only by a build of the same
  scope. A build without a scope (a Linux host node, whose key comes from a file)
  has the scope of eight zero bytes.
- An instance may keep a rotated key with its own store hook (a host rewrites
  its key file) instead of the platform keystore.

## Consequences

- Reflashing a board, or moving it to another mesh, always takes effect, even
  when an older or foreign key is left in the keystore.
- The scope reveals nothing usable about K_gw, but it does tell two meshes'
  boards apart to whoever can read the keystore.
- The keystore stores the scope opaquely and covers it with its CRC
  (robohub-common COMM-0006).
