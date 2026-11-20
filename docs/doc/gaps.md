# The gap list — robohub-node Phase 0

Each requirement of the node programming model (`docs/requirements/node.yml`,
RNODE-0001…0012) against what the ecosystem has on 2026-11-20. **Have** means it
exists and only moves in Phase 1; **partial** means it exists in part or in the
wrong place; **missing** means nothing does it yet. The phase is where it is
planned (robohub ADR-0020).

| Requirement | State | Where it is today | What is missing | Phase |
|---|---|---|---|---|
| RNODE-0001 one cooperative loop | **have** | robohub-core: `app.h` (lifecycle), `scheduler.h` (periodic tasks), `core.h` | nothing but the move | 1 |
| RNODE-0002 synchronous zero-copy events | **have** | robohub-core: `event.h`, `events.h` | nothing but the move | 1 |
| RNODE-0003 layers, upward only | **partial** (Phase 1: the collection is in place) | the dependency chain already points up (transport → core, telemetry → transport, core); device-node sits in robohub-devices | the collection; the core-only image test on the smallest board | 1 |
| RNODE-0004 chosen in the manifest | **partial** | each service is a separate module a manifest names by path; core tables via Kconfig | one place in an application's manifest to choose services; the proof that an unchosen one is absent | 1 |
| RNODE-0005 standalone first-class | **partial** | no-runtime applications build today (Pico hello world in C and Rho; the DS18B20 Rho reader); core-only C examples (DS18B20 bare metal) | a core-only example in Rho; a documented path from one tier to the next with no handler change | 1 |
| RNODE-0006 no allocation after init | **partial** | robohub-core's tables are fixed and full is `ROBOHUB_ERR_NO_MEM`; Rho's board arena is fixed | high-water marks in the diagnostics; a check that transport and telemetry allocate nothing after init | 1 |
| RNODE-0007 signals and actions named once | **partial** | manifests declare signals and actions; Mesh reads them; on the board they are per-driver code | a core service that reads and drives by manifest name; telemetry and link using the same names | 1–2 |
| RNODE-0008 watchdog, reset reasons, fault record | **have** (2a, 2026-11-20) | robohub-common 1.3.0 `robohub/health.h`; core 0.2.0 `robohub/core/health.h` (policy, host-tested); device-node 0.2.0 `HEALTH`; Pico, ESP32, STM32, AVR implementations | on-target checks on each board | 2a |
| RNODE-0009 behaviour-preserving adapters | **partial** | bare-metal defaults and robohub-freertos implement the OSAL | `kernels/freertos` in the collection; a test running one application on both | 1 |
| RNODE-0010 Rho bindings for the core | **missing** | Rho board applications call the platform and drivers through FFI and loop by themselves | bindings published by the collection; a Rho node example | 1 |
| RNODE-0011 secure link, shared vectors | **partial** | `link/`: `secure.c`; `test_vectors.c` already checks the board against the shared known-answer vectors (`tests/vectors/secure.json`) robohub-gateway uses | unsealed frames refused on a network link by the runtime itself | 1–2 |
| RNODE-0012 managed lifecycle from the edge | **partial** | enrollment and node keys through the keeper; Mesh builds, flashes and tracks deployed signatures | signed A/B updates over the link with rollback; health reports; per-MCU keystore backends (the contract exists, Linux implements it) | 2 |

## Moves in Phase 1 (the collection)

| From | To (module in robohub-node) | Notes |
|---|---|---|
| robohub-core | `core/` | names, id, version, CHANGELOG and prefix CORE kept |
| robohub-transport | `link/` | prefix TRAN kept; sealing and its vectors with it |
| robohub-telemetry | `telemetry/` | prefix TELE kept |
| robohub-devices/runtime/device-node | `device-node/` | leaves robohub-devices; its requirements (DEVS-NODE-*) move with it, ids kept. `nodes/gateway-node` stays in robohub-devices: it is an application of devices' drivers, and moving it would make the collection depend on robohub-devices |
| robohub-freertos | `kernels/freertos/` | prefix RTOS kept; the kernel still comes from the platform |

Touched by the move: 14 consumer manifests (paths only), the CI sibling lists of
robohub, robohub-devices, robohub-drivers, robohub-pico, robohub-telemetry and
robohub-transport, robohub's four registry entries, the docsite's site list, and
seven documentation pages.

## Decided after Phase 0

- The first real node: the settlement's temperature and humidity sensor, end to
  end to the widget on morlaca.ro (Phase 3).
- The third kernel: Zephyr (Phase 3); where its adapter lives is decided when it
  is written.
