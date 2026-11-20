/*
 * health_fixture.h — a fake board for device-node's health tests: a clock the
 * loop advances, a link that keeps every frame the node writes, reset flags and
 * retained memory. One program per scenario, because the runtime starts once.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#ifndef DEVICE_NODE_HEALTH_FIXTURE_H
#define DEVICE_NODE_HEALTH_FIXTURE_H

#include "device_node.h"
#include "protocol.h"
#include "registry.h"
#include "robohub/health.h"
#include "robohub/core/app.h"
#include "robohub/core/core.h"
#include "robohub/core/scheduler.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

/* The clock. */
static rh_u32 fx_clock;
static rh_millis fx_millis(RoboHubTimer* t) { (void)t; return fx_clock; }
static void fx_delay(RoboHubTimer* t, rh_u32 ms) { (void)t; fx_clock += ms; }
static RoboHubTimer fx_timer = { .name = "system_timer", .get_millis = fx_millis, .delay_ms = fx_delay };

/* Reset flags and retained memory, as a platform provides them. */
static rh_u32 fx_retained[RH_HEALTH_RETAINED_SIZE / 4u];
static rh_reset_reason fx_flags = RH_RESET_SOFTWARE;
void* rh_platform_retained(void) { return fx_retained; }
rh_reset_reason rh_platform_reset_reason(void) { return fx_flags; }

/* The link: every byte the node writes, and what the gateway sends next. */
static rh_u8 fx_wire[16384];
static rh_size fx_wire_len;
static rh_u8 fx_inbox[256];
static rh_size fx_inbox_len;

static int fx_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    assert(fx_wire_len + length <= sizeof fx_wire);
    memcpy(fx_wire + fx_wire_len, data, length);
    fx_wire_len += length;
    return 0;
}

static rh_size fx_read(rh_u8* out, rh_size capacity, void* context)
{
    (void)context;
    rh_size n = fx_inbox_len < capacity ? fx_inbox_len : capacity;
    memcpy(out, fx_inbox, n);
    memmove(fx_inbox, fx_inbox + n, fx_inbox_len - n);
    fx_inbox_len -= n;
    return n;
}

/* One frame off the wire: COBS-decoded, the header split off. */
typedef struct {
    rh_u16 msg_id;
    rh_u16 tx_id;
    rh_u16 length;
    rh_u8  payload[128];
} fx_frame;

/* The frames on the wire, in order; returns how many. */
static int fx_frames(fx_frame* out, int capacity)
{
    int count = 0;
    rh_size start = 0;
    for (rh_size i = 0; i < fx_wire_len && count < capacity; i++) {
        if (fx_wire[i] != 0u) {
            continue;
        }
        rh_u8 decoded[256];
        rh_size n = 0;
        rh_size p = start;
        while (p < i) {
            rh_u8 code = fx_wire[p++];
            for (rh_u8 k = 1; k < code && p < i; k++) {
                decoded[n++] = fx_wire[p++];
            }
            if (code != 0xFFu && p < i) {
                decoded[n++] = 0u;
            }
        }
        start = i + 1;
        if (n < 8u) {
            continue;
        }
        fx_frame* f = &out[count++];
        f->msg_id = (rh_u16)(decoded[0] | (decoded[1] << 8));
        f->tx_id = (rh_u16)(decoded[2] | (decoded[3] << 8));
        f->length = (rh_u16)(decoded[4] | (decoded[5] << 8));
        assert(f->length <= sizeof f->payload && 6u + f->length + 2u <= n);
        memcpy(f->payload, decoded + 6, f->length);
    }
    return count;
}

static rh_u32 fx_u32(const rh_u8* p) { return (rh_u32)p[0] | ((rh_u32)p[1] << 8) | ((rh_u32)p[2] << 16) | ((rh_u32)p[3] << 24); }
static rh_u16 fx_u16(const rh_u8* p) { return (rh_u16)(p[0] | (p[1] << 8)); }

/* A frame for the node to read, framed by a transport of the test's own. */
static rh_u8 fx_encoded[256];
static rh_size fx_encoded_len;
static int fx_encode_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    memcpy(fx_encoded + fx_encoded_len, data, length);
    fx_encoded_len += length;
    return 0;
}

static void fx_send_to_node(rh_u16 msg_id, const void* payload, rh_u16 length)
{
    static RoboHubTransport encoder;
    RoboHubTransportPhy phy = { .write = fx_encode_write, .context = 0 };
    fx_encoded_len = 0;
    assert(robohub_transport_init(&encoder, &phy) == ROBOHUB_OK);
    assert(robohub_transport_send(&encoder, msg_id, 0u, payload, length) == ROBOHUB_OK);
    memcpy(fx_inbox + fx_inbox_len, fx_encoded, fx_encoded_len);
    fx_inbox_len += fx_encoded_len;
}

#endif /* DEVICE_NODE_HEALTH_FIXTURE_H */
