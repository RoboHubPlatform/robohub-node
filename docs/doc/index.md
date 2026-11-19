# Telemetry

Binds the runtime's event bus to the outside. A program publishes a reading and
is done; telemetry subscribes to the events it names, thins them, and writes each
one to whatever channels are registered.

```
robohub-core        synchronous event bus
      │  events & logs
robohub-telemetry   filter · decimator · router
      ├── robohub-transport   COBS frames over UART / USB CDC / RS-485
      └── robohub/net.h       length-framed packets over TCP / UDP (robohub-common)
```

## Responsibility

- **Automatic bridging.** A driver publishes `EVENT_MOTOR_TELEMETRY` and nothing
  else; telemetry is what puts it on the wire. No driver knows about a socket.
- **Decimation.** A 1 kHz control loop overflows a 115200 baud link. Each event
  is bridged with its own rate — `robohub_telemetry_enable_event(id, N)` sends
  one in N — so a fast signal is thinned and a slow one passes whole.
- **Multi-sink routing.** The same frame goes to every enabled sink at once:
  raw binary over USB CDC for the edge controller and a thinned copy over Wi-Fi,
  from one publish.
- **Log interception.** `robohub_telemetry_log` sends `level tag: message` to the
  same sinks, under the log event, so a console and a cloud dashboard see the
  same line.

## API

```c
robohub_telemetry_init();
robohub_telemetry_add_sink(&(RoboHubTelemetrySink){
    .type = ROBOHUB_SINK_TRANSPORT, .handle.transport = &usb, .enabled = true });
robohub_telemetry_enable_event(EVENT_MOTOR_TELEMETRY, 1);   /* every event */
robohub_telemetry_enable_event(EVENT_IMU, 10);              /* one in ten */
robohub_telemetry_log(2, "motor", "overcurrent");
```

A sink is `ROBOHUB_SINK_TRANSPORT` (a `robohub-transport` link, see
{ref}`robohub-transport/index`) or `ROBOHUB_SINK_SOCKET` (a network socket from
`robohub-common`'s `robohub/net.h`, plain frames only — see below). It carries bytes. The payload is the packed struct a rho schema generated; the
sink adds framing, not meaning.

## Socket sinks and an authenticated link

A socket sink writes plain frames: it has no session to seal under, and no way
to hear a gateway's answer to a hello. It is for a local collector — a bench
tool on the same network as a board whose link is not authenticated. While any
sink's transport is secure (`robohub_transport_secure`), a socket sink is
refused (`robohub_telemetry_add_sink` answers `ROBOHUB_ERR_STATE`), and one
registered before the link was secured writes nothing; each frame it does not
write is counted in `frames_dropped`. A plain copy of every reading beside the
sealed one would be refused by the gateway on a network link anyway, and is one
anyone on that network could forge without being told apart.

A board that reports to a gateway over IP does not use a socket sink: it uses a
transport sink whose link writes to the socket (a `RoboHubTransportPhy` over
`robohub_socket_send`), so its frames are sealed exactly as a serial link's are
and the gateway's answers come back through it — as the ds18b20 telemetry
example in robohub-drivers does.

## Counters

`robohub_telemetry_stats` reports `frames_routed` and `frames_dropped`, so a link
that is failing says so rather than going quiet.

## Network devices

A telemetry sink is a byte stream. An IP device — a Shelly plug, for example —
uses a network client instead: its integration publishes over HTTP RPC or MQTT
and, behind a relay, posts the encoded frame to the gateway's
`/api/v1/frames`. The transport capabilities include `http` and `mqtt`.
