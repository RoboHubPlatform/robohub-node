/*
 * transport.c — COBS framing, a Fletcher-16 check, and a byte-at-a-time parser.
 *
 * The parser is a few states worth of work per byte and no allocation: bytes
 * land in the instance's receive buffer, a complete frame is decoded onto the
 * stack, and a frame that checks out is published on the event bus before
 * `feed_byte` returns. Nothing here blocks, so it is safe to call from the
 * interrupt that delivered the byte.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "robohub/transport.h"
#include "robohub/core/event.h"
#include "robohub/core/health.h"

#include <string.h>

/* A COBS frame adds one length byte per 254 payload bytes and one for the run
 * still open at the end, so an encoded frame is at most this wide. */
#define RH_TRANSPORT_ENCODED_MAX \
    (RH_TRANSPORT_MAX_FRAME_SIZE + (RH_TRANSPORT_MAX_FRAME_SIZE / 254u) + 1u)

/* -------------------------------------------------------------------------- */
/*  Integrity                                                                 */
/* -------------------------------------------------------------------------- */

/*
 * Fletcher-16: two running sums modulo 255, concatenated. It costs an add and a
 * compare per byte and catches the errors a serial line actually makes — a
 * flipped bit, a dropped byte, a transposition — where a table-driven CRC would
 * cost a table this library would have to carry.
 */
// @req TRAN-FRAME-0001
static rh_u16 fletcher16(const rh_u8* data, rh_size length)
{
    rh_u16 sum1 = 0u;
    rh_u16 sum2 = 0u;
    for (rh_size i = 0; i < length; i++) {
        sum1 = (rh_u16)((sum1 + data[i]) % 255u);
        sum2 = (rh_u16)((sum2 + sum1) % 255u);
    }
    return (rh_u16)((sum2 << 8) | sum1);
}

/* -------------------------------------------------------------------------- */
/*  COBS                                                                      */
/* -------------------------------------------------------------------------- */

/* Encode into `output`, which must hold RH_TRANSPORT_ENCODED_MAX bytes. Returns
 * the encoded length; the caller appends the delimiter. */
// @req TRAN-FRAME-0001
static rh_size cobs_encode(const rh_u8* input, rh_size length, rh_u8* output)
{
    rh_size read_index = 0u;
    rh_size write_index = 1u;
    rh_size code_index = 0u;
    rh_u8 code = 1u;

    while (read_index < length) {
        if (input[read_index] == 0u) {
            output[code_index] = code;
            code = 1u;
            code_index = write_index++;
            read_index++;
        } else {
            output[write_index++] = input[read_index++];
            code++;
            if (code == 0xFFu) {
                output[code_index] = code;
                code = 1u;
                code_index = write_index++;
            }
        }
    }
    output[code_index] = code;
    return write_index;
}

/* Decode into `output`, which must hold at least `length` bytes. Returns the
 * decoded length, or 0 when the input is truncated or malformed. */
static rh_size cobs_decode(const rh_u8* input, rh_size length, rh_u8* output, rh_size capacity)
{
    rh_size read_index = 0u;
    rh_size write_index = 0u;

    while (read_index < length) {
        rh_u8 code = input[read_index++];
        if (code == 0u) {
            return 0u; /* a zero code has no meaning inside a COBS frame */
        }
        for (rh_u8 i = 1u; i < code; i++) {
            if (read_index >= length || write_index >= capacity) {
                return 0u;
            }
            output[write_index++] = input[read_index++];
        }
        /* A code short of 0xFF stands for the zero it replaced, unless it is the
         * final code. */
        if (code < 0xFFu && read_index < length) {
            if (write_index >= capacity) {
                return 0u;
            }
            output[write_index++] = 0x00u;
        }
    }
    return write_index;
}

/* -------------------------------------------------------------------------- */
/*  Little-endian helpers                                                     */
/* -------------------------------------------------------------------------- */

static void put_u16(rh_u8* out, rh_u16 value)
{
    out[0] = (rh_u8)(value & 0xFFu);
    out[1] = (rh_u8)((value >> 8) & 0xFFu);
}

static rh_u16 get_u16(const rh_u8* in)
{
    return (rh_u16)((rh_u16)in[0] | ((rh_u16)in[1] << 8));
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

RoboHubError robohub_transport_init(RoboHubTransport* transport,
                                    const RoboHubTransportPhy* phy)
{
    if (transport == 0 || phy == 0 || phy->write == 0) {
        return ROBOHUB_ERR_INVALID_ARG;
    }
    memset(transport, 0, sizeof(*transport));
    transport->phy = *phy;
    return ROBOHUB_OK;
}

/* transport_hello says a HELLO, unless one is waiting for its answer and was
 * said less than RH_TRANSPORT_HELLO_RETRY_MS ago. */
static void transport_hello(RoboHubTransport* transport);

/* HEALTH once per boot, when the link can send it. */
static void transport_health_once(RoboHubTransport* transport);

/* A frame that just finished is decoded, checked and, if it passes, published.
 * Anything wrong with it is counted and dropped: a stream has no way to ask for
 * the bytes again, and a half-frame is not worth keeping. */
// @req TRAN-0002, TRAN-FRAME-0002, TRAN-FRAME-0003, TRAN-SEC-0008, TRAN-SEC-0011
static void transport_deliver(RoboHubTransport* transport)
{
    rh_u8 frame[RH_TRANSPORT_MAX_FRAME_SIZE];
    rh_size decoded = cobs_decode(transport->rx, transport->rx_length, frame, sizeof(frame));

    if (decoded < RH_TRANSPORT_MIN_FRAME) {
        transport->dropped++;
        return;
    }

    rh_u16 msg_id = get_u16(&frame[0]);
    rh_u16 tx_id = get_u16(&frame[2]);
    rh_u16 payload_len = get_u16(&frame[4]);

    if ((rh_size)RH_TRANSPORT_HEADER_SIZE + payload_len + RH_TRANSPORT_CHECK_SIZE != decoded) {
        transport->dropped++;
        return;
    }

    rh_u16 expected = get_u16(&frame[RH_TRANSPORT_HEADER_SIZE + payload_len]);
    rh_u16 actual = fletcher16(frame, (rh_size)RH_TRANSPORT_HEADER_SIZE + payload_len);

    if (expected != actual) {
        transport->check_errors++;
        return;
    }

    const rh_u8* payload = &frame[RH_TRANSPORT_HEADER_SIZE];
    rh_size length = payload_len;
    if (transport->secure != 0) {
        /* An authenticated link accepts the gateway's answer to its hello and
         * frames that open under the session, and nothing else. */
        if (msg_id == RH_SECURE_MSG_SESSION) {
            if (robohub_secure_accept(transport->secure, payload, length) == ROBOHUB_OK) {
                transport_health_once(transport);
            }
            return;
        }
        const rh_u8* inner = 0;
        rh_size inner_length = 0u;
        if (robohub_secure_open(transport->secure, msg_id, tx_id, payload, length,
                                &inner, &inner_length) != ROBOHUB_OK) {
            transport->dropped++;
            return;
        }
        msg_id = (rh_u16)(msg_id & (rh_u16)~RH_SECURE_FLAG);
        payload = inner;
        length = inner_length;
        /* A key rotation is the secure layer's own: the new key is stored and
         * the answer goes back under the same session, and nothing above this
         * layer sees either. */
        if (msg_id == RH_SECURE_MSG_REKEY) {
            rh_u8 ack[RH_SECURE_REKEY_ACK_SIZE];
            if (robohub_secure_rekey(transport->secure, payload, length, ack) == ROBOHUB_OK) {
                (void)robohub_transport_send(transport, RH_SECURE_MSG_REKEY_ACK, tx_id, ack,
                                             (rh_u16)sizeof(ack));
            }
            transport->rx_packets++;
            return;
        }
        /* The gateway asks for a new session — it restarted, and this one's
         * counter is below the mark it restored. It opened under the current
         * session, so it is the gateway's: the session is dropped and a hello
         * said now, so the next reading already has a session to go under. */
        if (msg_id == RH_SECURE_MSG_SESSION_RESET) {
            (void)robohub_secure_reset(transport->secure);
            transport->rx_packets++;
            transport_hello(transport);
            return;
        }
    }

    transport->rx_packets++;
    /* A diagnostic request asks for the health again (PROTO-0007); it still
     * goes on to whoever else answers it. */
    if (msg_id == RH_TRANSPORT_MSG_DIAG_REQUEST && transport->health_on) {
        (void)robohub_transport_send_health(transport);
    }
    if (transport->on_frame != 0 &&
        transport->on_frame(msg_id, tx_id, payload, length, transport->on_frame_user) != 0) {
        return;
    }
    (void)robohub_core_event_publish_with_tx((robohub_event_id)msg_id, tx_id,
                                             payload, (rh_u16)length);
}

// @req TRAN-FRAME-0002, TRAN-FRAME-0005
void robohub_transport_feed_byte(RoboHubTransport* transport, rh_u8 byte)
{
    if (byte == RH_TRANSPORT_DELIMITER) {
        if (transport->rx_length > 0u) {
            transport_deliver(transport);
            transport->rx_length = 0u;
        }
        return;
    }

    if (transport->rx_length < RH_TRANSPORT_MAX_FRAME_SIZE) {
        transport->rx[transport->rx_length++] = byte;
    } else {
        /* The frame is longer than any frame can be, so there is no point
         * keeping the rest of it: drop what is held and let the next delimiter
         * start clean. */
        transport->dropped++;
        transport->rx_length = 0u;
    }
}

void robohub_transport_feed(RoboHubTransport* transport, const rh_u8* data, rh_size length)
{
    if (transport == 0 || data == 0) {
        return;
    }
    for (rh_size i = 0; i < length; i++) {
        robohub_transport_feed_byte(transport, data[i]);
    }
}

RoboHubError robohub_transport_secure(RoboHubTransport* transport, RoboHubSecure* secure)
{
    if (transport == 0 || secure == 0) {
        return ROBOHUB_ERR_INVALID_ARG;
    }
    transport->secure = secure;
    return ROBOHUB_OK;
}

// @req TRAN-0006, TRAN-FRAME-0006
RoboHubError robohub_transport_on_frame(RoboHubTransport* transport,
                                        RoboHubTransportFrameHook hook, void* user)
{
    if (transport == 0) {
        return ROBOHUB_ERR_INVALID_ARG;
    }
    transport->on_frame = hook;
    transport->on_frame_user = user;
    return ROBOHUB_OK;
}

/* transport_write frames one payload as it is and writes it to the link. */
static RoboHubError transport_write(RoboHubTransport* transport, rh_u16 msg_id, rh_u16 tx_id,
                                    const void* payload, rh_u16 payload_len);

/* A HELLO is repeated every two seconds until it is answered: a hello lost on a
 * noisy line must not leave the board waiting for an answer that cannot come. */
#define RH_TRANSPORT_HELLO_RETRY_MS 2000u

// @req TRAN-0002, TRAN-SEC-0007
RoboHubError robohub_transport_send(RoboHubTransport* transport,
                                    robohub_event_id msg_id,
                                    rh_u16 tx_id,
                                    const void* payload,
                                    rh_u16 payload_len)
{
    if (transport == 0 || transport->phy.write == 0) {
        return ROBOHUB_ERR_STATE;
    }
    RoboHubSecure* secure = transport->secure;
    if (secure == 0) {
        /* A plain link has no session to wait for: the health goes first. */
        if ((rh_u16)msg_id != RH_TRANSPORT_MSG_HEALTH) {
            transport_health_once(transport);
        }
        return transport_write(transport, (rh_u16)msg_id, tx_id, payload, payload_len);
    }
    if (!robohub_secure_ready(secure)) {
        transport_hello(transport);
        return ROBOHUB_ERR_STATE;
    }
    rh_u8 sealed[RH_TRANSPORT_MAX_FRAME_SIZE];
    rh_size sealed_length = 0u;
    RoboHubError err = robohub_secure_seal(secure, (rh_u16)msg_id, tx_id, (const rh_u8*)payload,
                                           payload_len, sealed,
                                           RH_TRANSPORT_MAX_FRAME_SIZE - RH_TRANSPORT_HEADER_SIZE - RH_TRANSPORT_CHECK_SIZE,
                                           &sealed_length);
    if (err != ROBOHUB_OK) {
        return err;
    }
    return transport_write(transport, (rh_u16)(msg_id | RH_SECURE_FLAG), tx_id, sealed,
                           (rh_u16)sealed_length);
}

// @req TRAN-SEC-0007
static void transport_hello(RoboHubTransport* transport)
{
    RoboHubSecure* secure = transport->secure;
    rh_u32 now = secure->now_ms();
    if (secure->state != RH_SECURE_HELLO_SENT ||
        (rh_u32)(now - transport->hello_ms) >= RH_TRANSPORT_HELLO_RETRY_MS) {
        rh_u8 hello[RH_SECURE_HELLO_SIZE];
        if (robohub_secure_hello(secure, hello) == ROBOHUB_OK) {
            transport->hello_ms = now;
            (void)transport_write(transport, RH_SECURE_MSG_HELLO, 0u, hello, RH_SECURE_HELLO_SIZE);
        }
    }
}

// @req TRAN-0001, TRAN-FRAME-0001, TRAN-FRAME-0004
static RoboHubError transport_write(RoboHubTransport* transport, rh_u16 msg_id, rh_u16 tx_id,
                                    const void* payload, rh_u16 payload_len)
{
    if ((rh_size)RH_TRANSPORT_HEADER_SIZE + payload_len + RH_TRANSPORT_CHECK_SIZE
        > RH_TRANSPORT_MAX_FRAME_SIZE) {
        return ROBOHUB_ERR_INVALID_ARG;
    }

    rh_u8 frame[RH_TRANSPORT_MAX_FRAME_SIZE];
    put_u16(&frame[0], (rh_u16)msg_id);
    put_u16(&frame[2], tx_id);
    put_u16(&frame[4], payload_len);
    if (payload_len > 0u && payload != 0) {
        memcpy(&frame[RH_TRANSPORT_HEADER_SIZE], payload, payload_len);
    }
    rh_size body = (rh_size)RH_TRANSPORT_HEADER_SIZE + payload_len;
    put_u16(&frame[body], fletcher16(frame, body));
    body += RH_TRANSPORT_CHECK_SIZE;

    rh_u8 encoded[RH_TRANSPORT_ENCODED_MAX + 1u];
    rh_size encoded_len = cobs_encode(frame, body, encoded);
    encoded[encoded_len++] = RH_TRANSPORT_DELIMITER;

    if (transport->phy.write(encoded, encoded_len, transport->phy.context) != 0) {
        return ROBOHUB_ERR_IO;
    }
    transport->tx_packets++;
    return ROBOHUB_OK;
}

/* -------------------------------------------------------------------------- */
/*  Health (RNODE-HEALTH-0004, robohub-protocol PROTO-0007)                    */
/* -------------------------------------------------------------------------- */

static void put_u32_le(rh_u8* out, rh_u32 value)
{
    out[0] = (rh_u8)value;
    out[1] = (rh_u8)(value >> 8);
    out[2] = (rh_u8)(value >> 16);
    out[3] = (rh_u8)(value >> 24);
}

// @req RNODE-HEALTH-0004
void robohub_transport_health_payload(rh_u16 area, rh_u16 node, rh_u16 firmware,
                                      rh_u8 out[RH_TRANSPORT_HEALTH_SIZE])
{
    robohub_core_health health;
    robohub_core_health_get(&health);
    rh_bool fault = health.has_fault;
    out[0] = 1u;                       /* Health, version 1 */
    out[1] = 1u;                       /* DeviceId, version 1 */
    out[2] = (rh_u8)area;
    out[3] = (rh_u8)(area >> 8);
    out[4] = (rh_u8)node;
    out[5] = (rh_u8)(node >> 8);
    put_u32_le(&out[6], (rh_u32)health.reset_reason);
    put_u32_le(&out[10], health.boot_count);
    put_u32_le(&out[14], fault ? health.fault.code : 0u);
    rh_u16 task = fault ? health.fault.task : (rh_u16)RH_HEALTH_NO_TASK;
    out[18] = (rh_u8)task;
    out[19] = (rh_u8)(task >> 8);
    put_u32_le(&out[20], fault ? health.fault.pc : 0u);
    put_u32_le(&out[24], fault ? health.fault.uptime_ms : 0u);
    out[28] = (rh_u8)firmware;
    out[29] = (rh_u8)(firmware >> 8);
}

// @req RNODE-HEALTH-0004
RoboHubError robohub_transport_report_health(RoboHubTransport* transport,
                                             rh_u16 area, rh_u16 node, rh_u16 firmware)
{
    if (transport == 0) {
        return ROBOHUB_ERR_INVALID_ARG;
    }
    transport->health_on = RH_TRUE;
    transport->health_area = area;
    transport->health_node = node;
    transport->health_firmware = firmware;
    /* A plain link has no session to wait for, and one whose session is
     * already up has waited: either reports now. A write that fails (a network
     * not up yet) leaves it for the first frame sent. */
    if (transport->secure == 0 || robohub_secure_ready(transport->secure)) {
        transport_health_once(transport);
    }
    return ROBOHUB_OK;
}

// @req RNODE-HEALTH-0004
RoboHubError robohub_transport_send_health(RoboHubTransport* transport)
{
    if (transport == 0 || !transport->health_on) {
        return ROBOHUB_ERR_STATE;
    }
    if (transport->secure != 0 && !robohub_secure_ready(transport->secure)) {
        return ROBOHUB_ERR_STATE;
    }
    rh_u8 payload[RH_TRANSPORT_HEALTH_SIZE];
    robohub_transport_health_payload(transport->health_area, transport->health_node,
                                     transport->health_firmware, payload);
    return robohub_transport_send(transport, RH_TRANSPORT_MSG_HEALTH, 0u, payload,
                                  (rh_u16)sizeof(payload));
}

static void transport_health_once(RoboHubTransport* transport)
{
    if (!transport->health_on || transport->health_sent) {
        return;
    }
    /* Marked first: the send below goes through robohub_transport_send. */
    transport->health_sent = RH_TRUE;
    if (robohub_transport_send_health(transport) != ROBOHUB_OK) {
        transport->health_sent = RH_FALSE;
    }
}
