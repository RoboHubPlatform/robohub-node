---
id: ADR-0002
title: The core feeds the watchdog only while tasks keep their time, and reports health once per boot
status: accepted
date: 2026-11-20
requirements: [RNODE-0008, RNODE-HEALTH-0001, RNODE-HEALTH-0002, RNODE-HEALTH-0003, RNODE-HEALTH-0004, RNODE-HEALTH-0005]
supersedes: []
superseded_by: []
---

# The core feeds the watchdog only while tasks keep their time, and reports health once per boot

## Context

robohub ADR-0021 puts health first in Phase 2: robohub-common declares a
watchdog, a reset reason and a fault record (COMM-0010..0012); each platform
implements them; the protocol gains `HEALTH` (PROTO-0007). The core decides
how they are used. Its scheduler is cooperative: a task that blocks blocks
everything, and a task that returns errors keeps running.

## Decision

1. **The watchdog period comes from the manifest's configuration**
   (`CONFIG_RH_CORE_WATCHDOG_MS` in `config.values`, 8000 by default when the core
   is chosen and the platform has a watchdog; 0 turns it off) — a Kconfig symbol
   like every other setting a board build reads, rather than a manifest section of
   its own that no tool reads (amended 2026-11-20).
2. **Feeding is earned per task.** The loop feeds the watchdog only while every
   registered task has run within its allowance — twice its period plus the
   watchdog period, or what the task declares. A loop that spins while one task
   is stuck (waiting on a bus that never answers) still resets the board.
3. **Health is read at boot, before applications.** The reset reason, the boot
   count and the fault record are read in the core's `init`; the fault record is
   taken (cleared) so it is reported once.
4. **Handlers write only the record.** A fault handler and a watchdog
   early-warning handler, where the platform has them, write the code, the
   running task, the program counter and the uptime, and nothing else.
5. **The report is a link-layer act.** With the link chosen, `HEALTH` goes out
   once per boot when the session is up and on request; the core itself never
   depends on the link (RNODE-0003).

## Consequences

- A task with a long period must declare its allowance, or it is given one
  generous enough; a task removed stops counting.
- Boards whose watchdog cannot be stopped keep it running through a soft reset;
  the core starts it only once per boot.
- The fault record is best-effort: a fault that corrupts the record's memory
  leaves a reset reason of fault and no detail.

## Alternatives considered

- **Feeding from the loop only** — simple, but a stuck task in a spinning loop
  never resets the board. Rejected.
- **A watchdog per task in hardware** — no chip offers enough of them. Rejected.
- **Reporting health in every DIAG_RESPONSE only** — no push; a reset between two
  polls would be missed. Rejected for the push of `HEALTH`.
