---
id: ADR-0001
title: The node programming model is one cooperative loop of handlers and timers, with opt-in layers
status: accepted
date: 2026-11-20
requirements: [RNODE-0001, RNODE-0002, RNODE-0003, RNODE-0004, RNODE-0005, RNODE-0006, RNODE-0007, RNODE-0008, RNODE-0009, RNODE-0010, RNODE-0011, RNODE-0012]
supersedes: []
superseded_by: []
---

# The node programming model is one cooperative loop of handlers and timers, with opt-in layers

## Context

robohub ADR-0020 makes robohub-node the node operating environment: a model and
runtime services over existing kernels, kept as one collection, with board
applications that may use none, some or all of it. Phase 0 writes the model down
before any module moves in.

What already runs on boards:

- **robohub-core** — applications with `init`/`start`/`step`/`stop` (every `init`
  before any `start`, every `stop` in reverse); periodic tasks (a function and a
  period, cooperative, no preemption: "a task that blocks blocks everything", in
  exchange for no stack per task and no locking); an event bus (zero-copy,
  synchronous, subscription order, a refusing subscriber counted and not
  stopping the others); diagnostics; fixed tables (`RH_CORE_MAX_TASKS` 8,
  `RH_CORE_MAX_SUBSCRIBERS` 16, `RH_CORE_MAX_APPS`) set through Kconfig.
- **robohub-common's OSAL** — `robohub_osal_rtos_present`, `start`, threads,
  `sleep_ms`, `now_ms`, mutexes; bare-metal defaults, FreeRTOS in robohub-freertos.
- **robohub-transport** — framing, the integrity check, sealing (`secure.c`) and
  test vectors; **robohub-telemetry** — routing and decimation;
  **device-node** — a device behind a board.
- **Rho on boards** — applications on the platform and drivers through FFI, with
  their own loop (a DS18B20 reader); no Rho binding to robohub-core.

## Decision

1. **One cooperative loop is the model** (RNODE-0001, -0002): applications with
   a lifecycle, periodic tasks, and synchronous zero-copy events — robohub-core's
   semantics, kept exactly. Threads are not part of the model; a kernel adapter
   may offer them to the runtime's own services, never as what an application is
   written in.
2. **Layers, upward only** (RNODE-0003): core → link → telemetry → device node
   → managed, over kernel adapters, each depending only on robohub-common and the
   layers beneath. The core depends on nothing above it, and a test proves it on
   the smallest board.
3. **Chosen in the manifest** (RNODE-0004): an application's manifest names its
   services; Kconfig carries the choice to the build; an unchosen service is
   neither compiled nor linked.
4. **Standalone is first-class** (RNODE-0005): no runtime, core only, then
   networked and managed — and moving up a tier changes no handler.
5. **No allocation after init** (RNODE-0006): fixed tables and buffers from
   configuration; full is an error, not growth; high-water marks are reported.
6. **Signals and actions named once** (RNODE-0007): the manifest's names are
   what the core reads and drives locally, what telemetry publishes and what the
   link accepts.
7. **Health in the core** (RNODE-0008): a watchdog fed only while tasks keep
   their allowance, reset reasons and a fault record — new; nothing offers it
   today.
8. **Adapters are behaviour-preserving** (RNODE-0009): the same application
   behaves the same on bare metal and FreeRTOS, and a test runs it on both.
9. **Rho is a first-class way in** (RNODE-0010): published bindings for the core,
   so a Rho node application is handlers and timers without hand-written FFI.
10. **Secure by default, proven against the gateway** (RNODE-0011), and **a
    managed lifecycle from the edge** (RNODE-0012) when chosen.

## Consequences

- robohub-core's API stays the core's API; Phase 1 moves it, it does not rewrite
  it. New in the core: local signals and actions, settings use, the watchdog and
  reset records.
- A node application written today on robohub-core is already a core-tier
  application; one on the platform alone is a no-runtime application.
- The layering rule makes the link, telemetry and device node depend on the core
  through the event bus — as they already do.
- A Rho binding is a deliverable of Phase 1, not an afterthought.
- Applications that need preemption (a long blocking operation) use a kernel
  adapter's thread for that one service and hand results back as events — the
  pattern to document, not to build into the model.

## Alternatives considered

- **Threads as the model** (FreeRTOS tasks per component) — familiar, but a
  stack per task, locking everywhere, and nothing that runs on bare metal or an
  AVR. Rejected as the model; available inside a service.
- **An actor model with queued messages** — isolation between components, but
  copying and queues sized per actor on parts with a few kilobytes of RAM.
  Rejected for now; the synchronous bus covers today's nodes.
- **Services always compiled in, switched off at run time** — simpler builds,
  but networking code in every blink and no proof of the layering. Rejected.
