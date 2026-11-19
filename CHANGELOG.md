# Changelog

All notable changes to this module are recorded here, one section per release.

## [0.1.2] - 2026-11-19

A socket sink is refused (`ROBOHUB_ERR_STATE`) while a sink's transport is authenticated, and one registered before the link was secured writes nothing; its frames are counted as dropped. It wrote every reading and log line as a plain frame beside the sealed one: the gateway drops plain frames on a network link, and a plain copy is one anyone on that network could forge without being told apart. A board that reports over IP does so through a transport sink whose link writes to the socket, which seals as a serial link does; a board with no authenticated link keeps its socket sinks. The host test runs from `tests/tests.yml` (`rho test`).

## [0.1.1] - 2026-11-02

Documents the network device path: an IP integration publishes over HTTP RPC or
MQTT and a relay posts the encoded frame to the gateway.

## [0.1.0] - 2026-10-08

First release: event-bus sinks, per-event decimation and log interception.
