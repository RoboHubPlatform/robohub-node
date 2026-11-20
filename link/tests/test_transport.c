/*
 * test_transport.c — framing round-trips, COBS escaping, and a corrupt frame.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "robohub/transport.h"
#include "robohub/core/event.h"
#include "core_internal.h" /* the event bus initialiser, for a test with no runtime */
#include "keystore_double.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_EVENT ((robohub_event_id)0x1234u)

static int deliveries;
static robohub_event_id seen_id;
static rh_u16 seen_tx;
static rh_size seen_len;
static rh_u8 seen[32];

static RoboHubError on_event(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)user;
    deliveries++;
    seen_id = id;
    seen_tx = robohub_core_event_current_tx();
    seen_len = size;
    if (size <= sizeof(seen)) {
        memcpy(seen, payload, size);
    }
    return ROBOHUB_OK;
}

/* A link that is really the other end: what is written lands in `sink`. */
static int forward_write(const rh_u8* data, rh_size length, void* context)
{
    robohub_transport_feed((RoboHubTransport*)context, data, length);
    return 0;
}

/* A link that keeps what it was given, so a frame can be inspected and damaged. */
static rh_u8 captured[RH_TRANSPORT_MAX_FRAME_SIZE + 4u];
static rh_size captured_len;

static int capture_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    assert(length <= sizeof(captured));
    memcpy(captured, data, length);
    captured_len = length;
    return 0;
}


/* ---- an authenticated link ----------------------------------------------- */

static rh_u8 hello_seen[RH_SECURE_HELLO_SIZE];
static int hellos;
static RoboHubError on_hello(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    if (size == RH_SECURE_HELLO_SIZE) {
        memcpy(hello_seen, payload, size);
        hellos++;
    }
    return ROBOHUB_OK;
}

static int flagged;
static RoboHubError on_flagged(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)payload; (void)size; (void)user;
    flagged++;
    return ROBOHUB_OK;
}

/* The board's REKEY_ACK, as the gateway's end receives it (still sealed). */
static int acks;
static rh_u8 ack_seen[32];
static rh_size ack_len;
static RoboHubError on_ack(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    if (size <= sizeof(ack_seen)) {
        memcpy(ack_seen, payload, size);
        ack_len = size;
        acks++;
    }
    return ROBOHUB_OK;
}

/* A relay's hook: it takes a frame whole, the sealed id included. */
static int hooked;
static rh_u16 hooked_id;
static rh_u16 hooked_tx;
static rh_size hooked_len;
static int take_frames;
static int on_frame(rh_u16 msg_id, rh_u16 tx_id, const rh_u8* payload, rh_size length, void* user)
{
    (void)payload;
    assert(user == &hooked);
    hooked++;
    hooked_id = msg_id;
    hooked_tx = tx_id;
    hooked_len = length;
    return take_frames;
}

static rh_u32 now_ms_value = 5000u;
static rh_u32 test_now(void) { return now_ms_value; }
static int test_nonce(rh_u8* out, rh_size n) { for (rh_size i = 0; i < n; i++) out[i] = (rh_u8)(0x30u + i); return 0; }

/* The gateway's answer to a hello, from the protocol in secure.h. */
static void answer_hello(const rh_u8 k_dev[16], const rh_u8 hello[RH_SECURE_HELLO_SIZE], rh_u8 session[RH_SECURE_SESSION_SIZE])
{
    const rh_u8 nonce_g[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    memcpy(session, hello, 8);
    session[8] = 0x34u; session[9] = 0x12u;
    memcpy(session + 10, nonce_g, 8);
    session[18] = 0x10u; session[19] = 0x0Eu; /* 3600 s */
    rh_u8 material[11 + 8 + 8 + 2 + 8 + 2];
    memcpy(material, "rh-session1", 11);
    memcpy(material + 11, hello, 16);
    memcpy(material + 27, session + 8, 2);
    memcpy(material + 29, nonce_g, 8);
    memcpy(material + 37, session + 18, 2);
    robohub_secure_hmac(k_dev, 16, material, sizeof(material), session + 20, 8);
}

/* A frame the gateway seals for the board: sid, counter, payload and a tag
 * under k_down. Returns the sealed length. */
static rh_size seal_down(const rh_u8 k_down[16], rh_u16 flagged_id, rh_u16 sid, rh_u32 counter,
                         const rh_u8* payload, rh_size length, rh_u8* out)
{
    out[0] = (rh_u8)sid; out[1] = (rh_u8)(sid >> 8);
    out[2] = (rh_u8)counter; out[3] = (rh_u8)(counter >> 8);
    out[4] = (rh_u8)(counter >> 16); out[5] = (rh_u8)(counter >> 24);
    if (length > 0u) {
        memcpy(out + 6, payload, length);
    }
    rh_u8 tag_in[4 + 6 + 16];
    tag_in[0] = (rh_u8)flagged_id; tag_in[1] = (rh_u8)(flagged_id >> 8); tag_in[2] = 0u; tag_in[3] = 0u;
    memcpy(tag_in + 4, out, 6 + length);
    robohub_secure_hmac(k_down, 16, tag_in, 4 + 6 + length, out + 6 + length, 8);
    return 6u + length + 8u;
}

static int resets_seen;
static RoboHubError on_reset(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)payload; (void)size; (void)user;
    resets_seen++;
    return ROBOHUB_OK;
}

/* ---- HEALTH, once per boot and on request (RNODE-HEALTH-0004) ------------ */

static int healths;
static rh_u16 health_tx;
static rh_size health_len;
static rh_u8 health_seen[64];
static RoboHubError on_health(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    healths++;
    health_tx = robohub_core_event_current_tx();
    health_len = size;
    assert(size <= sizeof(health_seen));
    memcpy(health_seen, payload, size);
    return ROBOHUB_OK;
}

static int order_seen[8];
static int order_count;
static RoboHubError on_order_health(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)payload; (void)size; (void)user;
    if (order_count < 8) order_seen[order_count++] = 1;
    return ROBOHUB_OK;
}
static RoboHubError on_order_reading(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)payload; (void)size; (void)user;
    if (order_count < 8) order_seen[order_count++] = 2;
    return ROBOHUB_OK;
}

/* The payload as robohub-protocol's Health lays it out, with the core's health
 * as a program with no runtime has it: reason unknown, boot 0, no fault. */
static void check_health_payload(const rh_u8* p, rh_u16 area, rh_u16 node, rh_u16 firmware)
{
    assert(p[0] == 1u && p[1] == 1u);
    assert((rh_u16)(p[2] | (p[3] << 8)) == area && (rh_u16)(p[4] | (p[5] << 8)) == node);
    for (int i = 6; i < 18; i++) assert(p[i] == 0u);       /* reason, boot count, fault code */
    assert(p[18] == 0xFFu && p[19] == 0xFFu);              /* no task */
    for (int i = 20; i < 28; i++) assert(p[i] == 0u);      /* pc, uptime */
    assert((rh_u16)(p[28] | (p[29] << 8)) == firmware);
}

#define TEST_READING ((robohub_event_id)0x0204u)

// @verifies RNODE-HEALTH-0004
static void check_health(void)
{
    const rh_u8 reading[3] = { 1, 2, 3 };

    /* A plain link: the health goes out at once, and only once; a DIAG_REQUEST
     * asks for it again. */
    {
        RoboHubTransport gateway_end, board;
        const RoboHubTransportPhy to_gateway = { .write = forward_write, .context = &gateway_end };
        const RoboHubTransportPhy to_board = { .write = forward_write, .context = &board };
        assert(robohub_transport_init(&gateway_end, &to_board) == ROBOHUB_OK);
        assert(robohub_transport_init(&board, &to_gateway) == ROBOHUB_OK);
        assert(robohub_core_event_subscribe((robohub_event_id)RH_TRANSPORT_MSG_HEALTH, on_order_health, 0) == ROBOHUB_OK);
        assert(robohub_core_event_subscribe(TEST_READING, on_order_reading, 0) == ROBOHUB_OK);
        assert(robohub_core_event_subscribe((robohub_event_id)RH_TRANSPORT_MSG_HEALTH, on_health, 0) == ROBOHUB_OK);

        assert(robohub_transport_send_health(&board) == ROBOHUB_ERR_STATE);   /* not asked to report */
        assert(robohub_transport_report_health(&board, 7u, 20u, 9u) == ROBOHUB_OK);
        assert(healths == 1 && board.health_sent);                          /* at once */
        assert(robohub_transport_send(&board, TEST_READING, 0u, reading, 3u) == ROBOHUB_OK);
        assert(robohub_transport_send(&board, TEST_READING, 0u, reading, 3u) == ROBOHUB_OK);
        assert(healths == 1 && health_len == RH_TRANSPORT_HEALTH_SIZE && health_tx == 0u);
        assert(order_count == 3 && order_seen[0] == 1 && order_seen[1] == 2 && order_seen[2] == 2);
        check_health_payload(health_seen, 7u, 20u, 9u);

        const rh_u8 diag[9] = { 1, 1, 7, 0, 20, 0, 0, 0, 0 };
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)RH_TRANSPORT_MSG_DIAG_REQUEST, 3u, diag, 9u) == ROBOHUB_OK);
        assert(healths == 2);
        check_health_payload(health_seen, 7u, 20u, 9u);
    }

    /* An authenticated link: nothing before the session; the health goes out,
     * sealed, the moment the gateway's SESSION is accepted, and once. */
    {
        RoboHubTransport gateway_end, board;
        const RoboHubTransportPhy to_gateway = { .write = forward_write, .context = &gateway_end };
        const RoboHubTransportPhy to_board = { .write = forward_write, .context = &board };
        assert(robohub_transport_init(&gateway_end, &to_board) == ROBOHUB_OK);
        assert(robohub_transport_init(&board, &to_gateway) == ROBOHUB_OK);
        const rh_u8 uid[8] = { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8 };
        const rh_u8 k_dev[16] = { 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F, 0x10 };
        RoboHubSecure secure;
        assert(robohub_secure_init(&secure, uid, k_dev, test_nonce, test_now) == ROBOHUB_OK);
        assert(robohub_transport_secure(&board, &secure) == ROBOHUB_OK);
        assert(robohub_core_event_subscribe((robohub_event_id)(RH_TRANSPORT_MSG_HEALTH | RH_SECURE_FLAG), on_health, 0) == ROBOHUB_OK);
        assert(robohub_transport_report_health(&board, 7u, 21u, 4u) == ROBOHUB_OK);

        int before = healths;
        now_ms_value += 5000u;
        assert(robohub_transport_send(&board, TEST_READING, 0u, reading, 3u) == ROBOHUB_ERR_STATE); /* says hello */
        assert(healths == before && !board.health_sent);

        /* Answered under the key the board holds: the keystore may hold one the
         * REKEY above stored, which a board takes over the compiled-in one. */
        rh_u8 session[RH_SECURE_SESSION_SIZE];
        answer_hello(secure.device_key, hello_seen, session);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)RH_SECURE_MSG_SESSION, 0u, session, (rh_u16)sizeof(session)) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure));
        assert(board.health_sent);
        assert(healths == before + 1);
        assert(health_len == 6u + RH_TRANSPORT_HEALTH_SIZE + 8u);   /* sealed: sid, counter, health, tag */
        check_health_payload(health_seen + 6, 7u, 21u, 4u);

        /* Readings after it carry no second report. */
        assert(robohub_transport_send(&board, TEST_READING, 0u, reading, 3u) == ROBOHUB_OK);
        assert(healths == before + 1);
    }
}

int main(void)
{
    assert(robohub_core_event_init() == ROBOHUB_OK);
    assert(robohub_core_event_subscribe(TEST_EVENT, on_event, 0) == ROBOHUB_OK);

    /* A payload full of zeros and an 0xFF: the bytes COBS exists to hide. */
    const rh_u8 payload[] = { 0x01, 0x00, 0x02, 0x00, 0xFF, 0x00 };
    RoboHubTransport receiver;
    RoboHubTransport sender;
    const RoboHubTransportPhy link = { .write = forward_write, .context = &receiver };

    assert(robohub_transport_init(&receiver, &link) == ROBOHUB_OK);
    assert(robohub_transport_init(&sender, &link) == ROBOHUB_OK);

    assert(robohub_transport_send(&sender, TEST_EVENT, 42u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(deliveries == 1);
    assert(seen_id == TEST_EVENT);
    assert(seen_tx == 42u);
    assert(seen_len == sizeof(payload));
    assert(memcmp(seen, payload, sizeof(payload)) == 0);
    assert(sender.tx_packets == 1u);
    assert(receiver.rx_packets == 1u);
    assert(receiver.check_errors == 0u);

    /* The delimiter appears only at the end: that is what lets the parser find a
     * boundary without a timer. */
    RoboHubTransport capturer;
    const RoboHubTransportPhy capture = { .write = capture_write, .context = 0 };
    assert(robohub_transport_init(&capturer, &capture) == ROBOHUB_OK);
    assert(robohub_transport_send(&capturer, TEST_EVENT, 7u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(captured_len > 0u);
    assert(captured[captured_len - 1u] == RH_TRANSPORT_DELIMITER);
    for (rh_size i = 0; i + 1u < captured_len; i++) {
        assert(captured[i] != RH_TRANSPORT_DELIMITER);
    }

    /* A byte changed in flight is caught, counted, and not delivered. */
    int before = deliveries;
    captured[2] ^= 0x40u;
    robohub_transport_feed(&receiver, captured, captured_len);
    assert(deliveries == before);
    assert(receiver.check_errors == 1u);

    /* The old names are deprecated aliases of the new ones, so code written
     * against them still compiles and reads the same counter and size. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    assert(receiver.crc_errors == receiver.check_errors);
    assert(&receiver.crc_errors == &receiver.check_errors);
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
    assert(RH_TRANSPORT_CRC_SIZE == RH_TRANSPORT_CHECK_SIZE);
    assert(RH_TRANSPORT_MIN_FRAME == RH_TRANSPORT_HEADER_SIZE + RH_TRANSPORT_CHECK_SIZE);

    /* Noise longer than a frame is dropped rather than kept. */
    rh_u8 noise[RH_TRANSPORT_MAX_FRAME_SIZE + 8u];
    memset(noise, 0x5Au, sizeof(noise));
    robohub_transport_feed(&receiver, noise, sizeof(noise));
    assert(receiver.dropped >= 1u);

    /* A hook sees every frame that checks out before the bus does. One that
     * takes it keeps it off the bus, a sealed id as it came; one that leaves it
     * lets it through; a frame that does not check out never reaches it. */
    robohub_transport_feed_byte(&receiver, RH_TRANSPORT_DELIMITER); /* the noise's tail ends here */
    assert(robohub_transport_on_frame(&receiver, on_frame, &hooked) == ROBOHUB_OK);
    take_frames = 1;
    before = deliveries;
    assert(robohub_transport_send(&sender, (robohub_event_id)(TEST_EVENT | RH_SECURE_FLAG), 9u, payload,
                                  (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(hooked == 1 && hooked_id == (rh_u16)(TEST_EVENT | RH_SECURE_FLAG) && hooked_tx == 9u);
    assert(hooked_len == sizeof(payload));
    assert(deliveries == before);
    take_frames = 0;
    assert(robohub_transport_send(&sender, TEST_EVENT, 10u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(hooked == 2 && deliveries == before + 1);
    robohub_transport_feed(&receiver, captured, captured_len); /* still the damaged frame */
    assert(hooked == 2);
    assert(robohub_transport_on_frame(&receiver, 0, 0) == ROBOHUB_OK);
    assert(robohub_transport_send(&sender, TEST_EVENT, 11u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(hooked == 2 && deliveries == before + 2);

    /* An authenticated link sends nothing unauthenticated: the first send says
     * hello instead, a reading follows only once the session is up, it leaves
     * flagged, and a plain frame arriving is dropped. */
    assert(robohub_core_event_subscribe((robohub_event_id)RH_SECURE_MSG_HELLO, on_hello, 0) == ROBOHUB_OK);
    assert(robohub_core_event_subscribe((robohub_event_id)(TEST_EVENT | RH_SECURE_FLAG), on_flagged, 0) == ROBOHUB_OK);
    RoboHubTransport gateway_end;
    RoboHubTransport board;
    const RoboHubTransportPhy to_gateway = { .write = forward_write, .context = &gateway_end };
    const RoboHubTransportPhy to_board = { .write = forward_write, .context = &board };
    assert(robohub_transport_init(&gateway_end, &to_board) == ROBOHUB_OK);
    assert(robohub_transport_init(&board, &to_gateway) == ROBOHUB_OK);
    const rh_u8 uid[8] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8 };
    const rh_u8 k_dev[16] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0, 0x0F };
    RoboHubSecure secure;
    assert(robohub_secure_init(&secure, uid, k_dev, test_nonce, test_now) == ROBOHUB_OK);
    assert(robohub_transport_secure(&board, &secure) == ROBOHUB_OK);

    int delivered = deliveries;
    assert(robohub_transport_send(&board, TEST_EVENT, 1u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_ERR_STATE);
    assert(hellos == 1);
    assert(deliveries == delivered); /* the reading itself was not sent */
    assert(robohub_transport_send(&board, TEST_EVENT, 2u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_ERR_STATE);
    assert(hellos == 1); /* an unanswered hello is not repeated at once */
    now_ms_value += 2500u;
    assert(robohub_transport_send(&board, TEST_EVENT, 3u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_ERR_STATE);
    assert(hellos == 2); /* but is, two seconds later */

    rh_u8 session[RH_SECURE_SESSION_SIZE];
    answer_hello(k_dev, hello_seen, session);
    assert(robohub_transport_send(&gateway_end, (robohub_event_id)RH_SECURE_MSG_SESSION, 0u, session, (rh_u16)sizeof(session)) == ROBOHUB_OK);
    assert(robohub_secure_ready(&secure));

    assert(robohub_transport_send(&board, TEST_EVENT, 4u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(flagged == 1);              /* it arrived, flagged, at the gateway's end */
    assert(deliveries == delivered);   /* and was never delivered as a plain frame */

    rh_u32 dropped = board.dropped;
    assert(robohub_transport_send(&gateway_end, TEST_EVENT, 5u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
    assert(deliveries == delivered);   /* a plain frame to an authenticated board is dropped */
    assert(board.dropped == dropped + 1u);

    /* A REKEY sealed under the session is the secure layer's own: the board
     * stores the key and answers REKEY_ACK under the same session, and neither
     * reaches the event bus as a message of its own. */
    keystore_double_reset();
    assert(robohub_core_event_subscribe((robohub_event_id)(RH_SECURE_MSG_REKEY_ACK | RH_SECURE_FLAG), on_ack, 0) == ROBOHUB_OK);
    {
        rh_u8 material[8 + 8 + 8];
        rh_u8 k_down[16], mask[16];
        const rh_u8 nonce_g[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        memcpy(material, "rh-down1", 8);
        memcpy(material + 8, hello_seen + 8, 8);
        memcpy(material + 16, nonce_g, 8);
        robohub_secure_hmac(k_dev, 16, material, sizeof(material), k_down, 16);
        rh_u8 sealed[6 + RH_SECURE_REKEY_SIZE + 8];
        sealed[0] = 0x34u; sealed[1] = 0x12u;
        sealed[2] = 1u; sealed[3] = 0u; sealed[4] = 0u; sealed[5] = 0u;
        rh_u8 wrap_in[8 + 6];
        memcpy(wrap_in, "rh-wrap1", 8);
        memcpy(wrap_in + 8, sealed, 6);
        robohub_secure_hmac(k_dev, 16, wrap_in, sizeof(wrap_in), mask, 16);
        sealed[6] = 3u; sealed[7] = 0u; sealed[8] = 0u; sealed[9] = 0u;
        for (int i = 0; i < 16; i++) sealed[10 + i] = (rh_u8)(0xC0u + i) ^ mask[i];
        rh_u8 tag_in[4 + 6 + RH_SECURE_REKEY_SIZE];
        const rh_u16 flagged_id = (rh_u16)(RH_SECURE_MSG_REKEY | RH_SECURE_FLAG);
        tag_in[0] = (rh_u8)flagged_id; tag_in[1] = (rh_u8)(flagged_id >> 8); tag_in[2] = 0u; tag_in[3] = 0u;
        memcpy(tag_in + 4, sealed, 6 + RH_SECURE_REKEY_SIZE);
        robohub_secure_hmac(k_down, 16, tag_in, sizeof(tag_in), sealed + 6 + RH_SECURE_REKEY_SIZE, 8);
        int before = deliveries;
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)flagged_id, 0u, sealed, (rh_u16)sizeof(sealed)) == ROBOHUB_OK);
        assert(acks == 1);
        assert(ack_len == 6u + RH_SECURE_REKEY_ACK_SIZE + 8u);   /* sealed: sid, counter, ack, tag */
        assert(ack_seen[6] == 3u && ack_seen[7] == 0u);          /* the new epoch */
        assert(keystore_double_stores == 1 && keystore_double_key.epoch == 3u && keystore_double_key.key[0] == 0xC0u);
        assert(deliveries == before);
    }

    /* A SESSION_RESET is taken only sealed under the current session, with a
     * counter past the last accepted: then the session is dropped and a hello
     * said at once, and nothing above the secure layer sees it. */
    assert(robohub_core_event_subscribe((robohub_event_id)RH_SECURE_MSG_SESSION_RESET, on_reset, 0) == ROBOHUB_OK);
    assert(robohub_core_event_subscribe((robohub_event_id)(RH_SECURE_MSG_SESSION_RESET | RH_SECURE_FLAG), on_reset, 0) == ROBOHUB_OK);
    {
        rh_u8 material[8 + 8 + 8];
        rh_u8 k_down[16];
        const rh_u8 nonce_g[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        memcpy(material, "rh-down1", 8);
        memcpy(material + 8, hello_seen + 8, 8);
        memcpy(material + 16, nonce_g, 8);
        robohub_secure_hmac(k_dev, 16, material, sizeof(material), k_down, 16);
        const rh_u16 reset_id = (rh_u16)(RH_SECURE_MSG_SESSION_RESET | RH_SECURE_FLAG);
        const rh_u8 reason = 1u;
        rh_u8 sealed[6 + 1 + 8];
        rh_size n;
        int hellos_before = hellos;

        /* Unsealed: dropped, the session stands. */
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)RH_SECURE_MSG_SESSION_RESET, 0u, &reason, 1u) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure) && hellos == hellos_before);
        /* Under another session id. */
        n = seal_down(k_down, reset_id, 0x9999u, 2u, &reason, 1u, sealed);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure) && hellos == hellos_before);
        /* Under another key (the upward one). */
        rh_u8 k_wrong[16];
        memcpy(k_wrong, k_down, 16);
        k_wrong[0] ^= 0x01u;
        n = seal_down(k_wrong, reset_id, 0x1234u, 2u, &reason, 1u, sealed);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure) && hellos == hellos_before);
        /* A counter already used (the REKEY's): a replay. */
        n = seal_down(k_down, reset_id, 0x1234u, 1u, &reason, 1u, sealed);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure) && hellos == hellos_before);
        assert(resets_seen == 0);

        /* The real one, empty payload or a reason byte alike. */
        n = seal_down(k_down, reset_id, 0x1234u, 5u, &reason, 1u, sealed);
        int before = deliveries;
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(!robohub_secure_ready(&secure));
        assert(secure.state == RH_SECURE_HELLO_SENT);
        assert(hellos == hellos_before + 1);          /* the hello went out at once */
        assert(resets_seen == 0 && deliveries == before);
        /* The REKEY's key, stored before the reset, is the one the hello is under. */
        assert(secure.epoch == 3u && secure.device_key[0] == 0xC0u);

        /* The same reset again opens under no session. */
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(hellos == hellos_before + 1);
        /* A reading waits for the new session, and does not repeat the hello at once. */
        assert(robohub_transport_send(&board, TEST_EVENT, 6u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_ERR_STATE);
        assert(hellos == hellos_before + 1);

        rh_u8 next_session[RH_SECURE_SESSION_SIZE];
        answer_hello(secure.device_key, hello_seen, next_session);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)RH_SECURE_MSG_SESSION, 0u, next_session,
                                      (rh_u16)sizeof(next_session)) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure) && secure.tx_counter == 0u && secure.rx_counter == 0u);
        int flagged_before = flagged;
        assert(robohub_transport_send(&board, TEST_EVENT, 7u, payload, (rh_u16)sizeof(payload)) == ROBOHUB_OK);
        assert(flagged == flagged_before + 1);

        /* The old session's reset, replayed under the new session's id, does not
         * open: the new session has new keys. */
        n = seal_down(k_down, reset_id, 0x1234u, 6u, 0, 0u, sealed);
        assert(robohub_transport_send(&gateway_end, (robohub_event_id)reset_id, 0u, sealed, (rh_u16)n) == ROBOHUB_OK);
        assert(robohub_secure_ready(&secure));
    }

    check_health();
    return 0;
}
