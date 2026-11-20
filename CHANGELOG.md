# Changelog

## [0.5.0] - 2026-11-20

- **managed 0.2.0**: an image on trial gives up after five minutes unconfirmed (`CONFIG_RH_UPDATE_TRIAL_MS`, RNODE-UPD-0008) through `rh_update_reject()`, counted from when the layer first sees it pending (a board's clock may count from long before this boot). The end to end test adds a fourth case: an image that cannot be healthy before its limit, given up and reported rolled back.

## [0.4.0] - 2026-11-20

Phase 2b — firmware updates over the link (RNODE-UPD-0001…0007, ADR-0003; robohub ADR-0022):

- **managed 0.1.0**, a new module: takes `UPDATE_OFFER` (an Ed25519-signed manifest, checked with a vendored Monocypher 4.0.2 against the mesh's update key), refuses what is unsigned, for another board, a downgrade, too large or not updatable; writes `UPDATE_CHUNK`s in order to the inactive slot with up to four in flight, hashing as it goes and answering the next index it expects; resumes after a lost link; checks the SHA-256, says `verified` and boots the image once; confirms it once the session is up and every task has kept its time for 30 s, and reports `confirmed` or `rolled back`; drops a cancelled or silent transfer. Tested on a fake board, and end to end on a Linux host board through robohub-linux's file slot, with a lost chunk, a cut link, a refused downgrade and a crash rolled back.
- **core 0.3.0**: whether every task keeps its time is kept on a board without a watchdog too (`on_time`, `on_time_since_ms` in `robohub/core/health.h`) — the managed layer's "healthy".
- **link 0.6.0**: its SHA-256 is public.
- `NOTICE.md` names the vendored code.

## [0.3.0] - 2026-11-20

The link reports `HEALTH` itself (link 0.5.0, RNODE-HEALTH-0004), so any board with the link chosen reports its health — not only one built on the device node. device-node 0.2.1 hands its report to the link: one implementation, the same behaviour, its tests now passing through the link's.

## [0.2.0] - 2026-11-20

Phase 2a — health (RNODE-HEALTH-0001..0005, ADR-0002; robohub ADR-0021):

- **core 0.2.0**: health is read at boot before any application's `init` — the reset reason, the boot count and the fault record, taken once (`robohub/core/health.h`); the watchdog starts with `CONFIG_RH_CORE_WATCHDOG_MS` (8000, 0 off) on a platform that has one, and the loop feeds it only while every task has run within its allowance (twice its period plus the watchdog period, or `robohub_core_task_set_allowance`), in both the cooperative and the threaded loop; the scheduler tells robohub-common's fault record which task is running. Host tests on a fake board (`tests/test_health.c`).
- **device-node 0.2.0**: `HEALTH` (0x0403) once per boot — when the session is up on a sealed link, at once on a plain one — and again on a `DIAG_REQUEST`; `firmware_version` in the config; the protocol copies know `HEALTH`. Its first host tests, and a build that works on its own (it added its own folder again for every dependency).
- **link 0.4.3, telemetry 0.1.3, freertos 0.1.3, core, device-node**: dependencies are found inside the collection, and fetched from robohub-node rather than the archived repositories.

## [0.1.0] - 2026-11-20

Phase 1: the collection. robohub-core (`core/`), robohub-transport (`link/`), robohub-telemetry (`telemetry/`), robohub-freertos (`kernels/freertos/`) and robohub-devices' device-node runtime (`device-node/`) moved in, with their history; each keeps its module name, id, version, CHANGELOG and requirements prefix (CORE, TRAN, TELE, RTOS; device-node's become RNODE-DEV-0001..0003, formerly DEVS-NODE-0001..0003). Their dependencies name each other inside the collection. robohub-core's host tests are declared at last (`core/tests/tests.yml`, 12 tests), and one `rho test .` runs every module's suites. One CI workflow, one set of governance files.

## [0.0.1] - 2026-11-20

Phase 0: the node programming model, before any module moves in — requirements
RNODE-0001…0012, ADR-0001 (one cooperative loop of handlers and timers, with
opt-in layers), and the gap list against robohub-core, -transport, -telemetry,
-freertos and robohub-devices (robohub ADR-0020, SYS-0072).
