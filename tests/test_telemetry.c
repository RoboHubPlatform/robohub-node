/*
 * test_telemetry.c — decimation, multi-sink routing, and log interception.
 *
 * A transport sink is driven into a capture buffer, and the socket side of the
 * net contract is overridden with a capture too, so both channels are observed
 * without a board.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "robohub/telemetry.h"
#include "robohub/secure.h"
#include "robohub/core/event.h"
#include "core_internal.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#define TEST_EVENT ((robohub_event_id)0x2233u)

/* -------------------------------------------------------------------------- */
/*  Captures                                                                  */
/* -------------------------------------------------------------------------- */

static rh_u8 transport_bytes[1024];
static rh_size transport_len;
static int transport_frames;

static int capture_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    assert(transport_len + length <= sizeof(transport_bytes));
    memcpy(&transport_bytes[transport_len], data, length);
    transport_len += length;
    transport_frames++;
    return 0;
}

/* The socket side of the net contract, strongly, so it replaces the default. */
static rh_u8 socket_bytes[1024];
static rh_size socket_len;
static int socket_frames;

int robohub_socket_send(RoboHubSocket socket, const void* data, rh_size len)
{
    (void)socket;
    assert(socket_len + len <= sizeof(socket_bytes));
    memcpy(&socket_bytes[socket_len], data, len);
    socket_len += len;
    socket_frames++;
    return (int)len;
}

static bool contains(const rh_u8* haystack, rh_size haystack_len, const void* needle, rh_size needle_len)
{
    if (needle_len == 0u || needle_len > haystack_len) {
        return false;
    }
    for (rh_size i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(&haystack[i], needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* A socket sink writes plain frames: it has no session to seal under. On a
 * board whose link is authenticated, a plain copy of every reading next to the
 * sealed one is what the gateway refuses on a network link, and what anyone on
 * that network could forge without being told apart — so the socket sink is
 * refused while a sink's transport is secure, whichever was registered first. */
static int test_nonce(rh_u8* out, rh_size n) { for (rh_size i = 0; i < n; i++) out[i] = (rh_u8)(0x30u + i); return 0; }
static rh_u32 test_clock(void) { return 1000u; }

// @verifies TELE-0008
static void socket_refused_beside_a_secure_link(void)
{
    static const rh_u8 uid[RH_SECURE_UID_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const rh_u8 key[RH_SECURE_KEY_SIZE] = { 9 };
    const rh_u8 payload[] = { 0x5A, 0xA5 };

    /* Secure first: a socket sink is not taken. */
    assert(robohub_telemetry_init() == ROBOHUB_OK);
    RoboHubTransport link;
    RoboHubSecure secure;
    const RoboHubTransportPhy phy = { .write = capture_write, .context = 0 };
    assert(robohub_transport_init(&link, &phy) == ROBOHUB_OK);
    assert(robohub_secure_init(&secure, uid, key, test_nonce, test_clock) == ROBOHUB_OK);
    assert(robohub_transport_secure(&link, &secure) == ROBOHUB_OK);
    RoboHubTelemetrySink sealed = { .type = ROBOHUB_SINK_TRANSPORT, .handle.transport = &link, .enabled = RH_TRUE };
    RoboHubTelemetrySink plain = { .type = ROBOHUB_SINK_SOCKET, .handle.socket = (RoboHubSocket)1, .enabled = RH_TRUE };
    assert(robohub_telemetry_add_sink(&sealed) == ROBOHUB_OK);
    assert(robohub_telemetry_add_sink(&plain) == ROBOHUB_ERR_STATE);

    /* Socket first, the link secured after: the socket sink writes nothing,
     * readings and logs alike, and what it did not write is counted. */
    assert(robohub_telemetry_init() == ROBOHUB_OK);
    RoboHubTransport later;
    RoboHubSecure later_secure;
    assert(robohub_transport_init(&later, &phy) == ROBOHUB_OK);
    RoboHubTelemetrySink later_sink = { .type = ROBOHUB_SINK_TRANSPORT, .handle.transport = &later, .enabled = RH_TRUE };
    assert(robohub_telemetry_add_sink(&plain) == ROBOHUB_OK);
    assert(robohub_telemetry_add_sink(&later_sink) == ROBOHUB_OK);
    assert(robohub_secure_init(&later_secure, uid, key, test_nonce, test_clock) == ROBOHUB_OK);
    assert(robohub_transport_secure(&later, &later_secure) == ROBOHUB_OK);
    assert(robohub_telemetry_enable_event(TEST_EVENT, 1u) == ROBOHUB_OK);
    socket_frames = 0;
    socket_len = 0;
    assert(robohub_core_event_publish(TEST_EVENT, payload, (rh_size)sizeof(payload)) == ROBOHUB_OK);
    robohub_telemetry_log(1u, "motor", "overcurrent");
    assert(socket_frames == 0);
    RoboHubTelemetryStats stats;
    robohub_telemetry_stats(&stats);
    assert(stats.frames_dropped >= 2u);

    /* A board whose link is not authenticated keeps its socket sink: a local
     * collector on a bench gets every frame, as before. */
    assert(robohub_telemetry_init() == ROBOHUB_OK);
    RoboHubTransport open_link;
    assert(robohub_transport_init(&open_link, &phy) == ROBOHUB_OK);
    RoboHubTelemetrySink open_sink = { .type = ROBOHUB_SINK_TRANSPORT, .handle.transport = &open_link, .enabled = RH_TRUE };
    assert(robohub_telemetry_add_sink(&open_sink) == ROBOHUB_OK);
    assert(robohub_telemetry_add_sink(&plain) == ROBOHUB_OK);
    assert(robohub_telemetry_enable_event(TEST_EVENT, 1u) == ROBOHUB_OK);
    socket_frames = 0;
    socket_len = 0;
    assert(robohub_core_event_publish(TEST_EVENT, payload, (rh_size)sizeof(payload)) == ROBOHUB_OK);
    assert(socket_frames == 1);
}

// @verifies TELE-0001, TELE-0002, TELE-0003, TELE-0004, TELE-0005, TELE-0006
int main(void)
{
    assert(robohub_core_event_init() == ROBOHUB_OK);
    assert(robohub_telemetry_init() == ROBOHUB_OK);

    RoboHubTransport link;
    const RoboHubTransportPhy phy = { .write = capture_write, .context = 0 };
    assert(robohub_transport_init(&link, &phy) == ROBOHUB_OK);

    RoboHubTelemetrySink to_transport = {
        .type = ROBOHUB_SINK_TRANSPORT,
        .handle.transport = &link,
        .enabled = RH_TRUE,
    };
    RoboHubTelemetrySink to_socket = {
        .type = ROBOHUB_SINK_SOCKET,
        .handle.socket = (RoboHubSocket)1,
        .enabled = RH_TRUE,
    };
    assert(robohub_telemetry_add_sink(&to_transport) == ROBOHUB_OK);
    assert(robohub_telemetry_add_sink(&to_socket) == ROBOHUB_OK);

    /* One event in three reaches the sinks. */
    assert(robohub_telemetry_enable_event(TEST_EVENT, 3u) == ROBOHUB_OK);

    const rh_u8 payload[] = { 0x11, 0x22, 0x33 };
    for (int i = 0; i < 9; i++) {
        assert(robohub_core_event_publish(TEST_EVENT, payload, (rh_size)sizeof(payload)) == ROBOHUB_OK);
    }
    assert(transport_frames == 3);
    assert(socket_frames == 3);
    assert(contains(transport_bytes, transport_len, payload, sizeof(payload)));
    assert(contains(socket_bytes, socket_len, payload, sizeof(payload)));

    /* A log line goes to every sink under the log event. */
    robohub_telemetry_log(1u, "motor", "overcurrent");
    assert(transport_frames == 4);
    assert(socket_frames == 4);
    assert(contains(socket_bytes, socket_len, "overcurrent", 11u));

    RoboHubTelemetryStats stats;
    robohub_telemetry_stats(&stats);
    assert(stats.frames_routed == 8u); /* four frames, two sinks */
    assert(stats.frames_dropped == 0u);

    /* Disabling the rule stops the bridge. */
    assert(robohub_telemetry_disable_event(TEST_EVENT) == ROBOHUB_OK);
    assert(robohub_core_event_publish(TEST_EVENT, payload, (rh_size)sizeof(payload)) == ROBOHUB_OK);
    assert(transport_frames == 4);

    /* A transaction id reaches the sink. A request and its response share one,
     * and a bridge that framed every event with 0 would make every response
     * look unsolicited, so a host could never match an answer to its call. */
    assert(robohub_telemetry_enable_event(TEST_EVENT, 1u) == ROBOHUB_OK);
    socket_len = 0;
    socket_frames = 0;
    const rh_u8 tx_payload[] = { 0xAB, 0xCD };
    assert(robohub_core_event_publish_with_tx(TEST_EVENT, 0x1234u, tx_payload,
                                              (rh_size)sizeof(tx_payload)) == ROBOHUB_OK);
    assert(socket_frames == 1);
    assert(socket_len == 6u + sizeof(tx_payload) + 2u);
    assert(socket_bytes[2] == 0x34u && socket_bytes[3] == 0x12u);
    assert(memcmp(&socket_bytes[6], tx_payload, sizeof(tx_payload)) == 0);

    socket_refused_beside_a_secure_link();
    return 0;
}
