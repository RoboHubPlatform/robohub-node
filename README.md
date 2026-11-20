# robohub-node

The node operating environment of the RoboHub Platform: an application model —
applications with a lifecycle, periodic tasks and events on one cooperative loop
— and runtime services a board application opts into: local signals and actions,
settings, watchdog and reset records; a secure link; telemetry; the device node;
a lifecycle managed from the installation's edge — over bare metal, FreeRTOS or
another kernel, on [robohub-common](https://github.com/RoboHubPlatform/robohub-common)'s
contracts.

A board application may use none of it, the core alone, or all of it; moving up
changes no handler.

**Modules**: `core/` (event bus, scheduler, lifecycle, diagnostics), `link/`
(framing, sealing), `telemetry/`, `device-node/`, `kernels/freertos/` — moved in
from robohub-core, robohub-transport, robohub-telemetry, robohub-devices and
robohub-freertos with their history (robohub ADR-0020). The model:
[requirements](docs/requirements/node.yml), the [decision](docs/architecture/adr/),
the [gap list](docs/doc/gaps.md). `rho test .` runs every module's tests.

MIT licensed.
