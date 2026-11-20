# Changelog

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
