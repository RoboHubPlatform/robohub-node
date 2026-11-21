# RoboHub Node

robohub-node is the node operating environment of RoboHub: what makes a board a
RoboHub node. It is an application model — applications with a lifecycle,
periodic tasks and events, on one cooperative loop — and runtime services an
application opts into:

| Layer | What it gives | Needs a gateway |
|---|---|---|
| core | the loop, lifecycle, timers, events, local signals and actions, settings, watchdog and reset records, diagnostics | no |
| link | a secure byte link to a parent | yes |
| telemetry | signals published to sinks, decimated, buffered across an outage | yes, or a local sink |
| device node | equipment behind a board | yes |
| managed | signed firmware updates over the link, confirmed once healthy (`managed/`) | yes |
| kernel adapters | bare metal, FreeRTOS, later Zephyr | — |

It builds on robohub-common's contracts and on nothing platform-specific. A board
application may use no runtime at all (the platform's HAL alone), the core alone,
or everything; moving up changes no handler.

**Status — Phase 1.** The modules are in: `core/`, `link/`, `telemetry/`,
`device-node/` and `kernels/freertos/`, moved from robohub-core, robohub-transport,
robohub-telemetry, robohub-devices and robohub-freertos with their history. The
model: the requirements
(`docs/requirements/node.yml`), the decision (`docs/architecture/adr/0001-*`) and
the [gap list](gaps.md). The decision to build it this way is robohub ADR-0020.

**Phase 2a — health.** The core reads why the board restarted, its boot count and
the last fault record at boot, before any application, and keeps the watchdog
fed only while every task keeps its allowance (`CONFIG_RH_CORE_WATCHDOG_MS`,
8000 by default, 0 off; `robohub/core/health.h`). With the device node, the
board reports it once per boot in `HEALTH`. The requirements are
`docs/requirements/sub/health.yml` (RNODE-HEALTH-0001…0005), the decision
`docs/architecture/adr/0002-*`, and robohub ADR-0021 for the whole of Phase 2.

**Phase 2b — updates over the link.** The managed layer (`managed/`,
`CONFIG_RH_NODE_MANAGED`) takes an Ed25519-signed manifest, the image in chunks
to the inactive slot with up to four in flight, checks its SHA-256, boots it once
and keeps it only once the session is up and every task has kept its time for
30 s (`docs/requirements/sub/update.yml`, RNODE-UPD-0001…0007;
`docs/architecture/adr/0003-*`; robohub ADR-0022). It is tested on a fake board
and end to end on a Linux host board with robohub-linux's file slot. The chips'
own slots are Phase 2c (Pico 2 W) and 2d (ESP32).

**Phase 2e — one node template for every board.** `template/` is the firmware
Mesh builds for every node: the core, the link, telemetry and the managed layer,
with the sensors a plan fits written by Mesh into `src/sensors.c`, and none by
default. Each board's platform is a dependency of its own preset only (rho 0.21.0
`depends[].presets`, `c_connection.variants.<preset>.library`), so one manifest
builds for a Pico 2 W and for a LOLIN32 Lite:

```sh
rho build -target pico2w template
rho build -target lolin32-lite template
```

The template includes no SDK header. The board's console is reached as a link
through `robohub_console_write_raw` and `robohub_console_read_byte`, and the
network through `robohub/net.h`, which each platform implements. It reaches its
parent over a Wi-Fi socket, a UART or the console (`CONFIG_RH_NODE_TRANSPORT`).
It takes updates over that link when its build carries the mesh's update key.
The requirements are `docs/requirements/sub/template.yml` (RNODE-TPL-0001…0003);
the decision is robohub ADR-0025. `tests/template_test.py` builds both presets
as they are and as Mesh provisions them.
