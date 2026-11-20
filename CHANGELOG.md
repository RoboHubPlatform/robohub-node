# Changelog

## [0.1.0] - 2026-11-20

Phase 1: the collection. robohub-core (`core/`), robohub-transport (`link/`), robohub-telemetry (`telemetry/`), robohub-freertos (`kernels/freertos/`) and robohub-devices' device-node runtime (`device-node/`) moved in, with their history; each keeps its module name, id, version, CHANGELOG and requirements prefix (CORE, TRAN, TELE, RTOS; device-node's become RNODE-DEV-0001..0003, formerly DEVS-NODE-0001..0003). Their dependencies name each other inside the collection. robohub-core's host tests are declared at last (`core/tests/tests.yml`, 12 tests), and one `rho test .` runs every module's suites. One CI workflow, one set of governance files.

## [0.0.1] - 2026-11-20

Phase 0: the node programming model, before any module moves in — requirements
RNODE-0001…0012, ADR-0001 (one cooperative loop of handlers and timers, with
opt-in layers), and the gap list against robohub-core, -transport, -telemetry,
-freertos and robohub-devices (robohub ADR-0020, SYS-0072).
