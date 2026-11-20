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
| managed | identity, signed updates, health — run from the installation's edge | yes |
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
