# Monocypher 4.0.2, vendored

The Ed25519 verifier the managed layer checks an update manifest with
(robohub-node ADR-0003): `monocypher.c` and `monocypher.h`, and from the optional
directory `monocypher-ed25519.c` and `monocypher-ed25519.h` (Ed25519 with
SHA-512, as RFC 8032 defines it and as Go's `crypto/ed25519` signs). Unchanged.

- Source: <https://monocypher.org/download/monocypher-4.0.2.tar.gz>, taken
  2026-11-20, SHA-256
  `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`.
- Licence: dual, 2-clause BSD or CC0 1.0, at the user's choice (`LICENCE.md`).
- Checked: the RFC 8032 test vectors in `managed/tests/test_managed.c`.

A build links only what the managed layer calls; the rest of the library is
discarded with the image's unused sections.
