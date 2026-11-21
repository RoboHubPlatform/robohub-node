/**
 * @file transport.h
 * @brief Framing for a byte stream: COBS, a Fletcher-16 check, and a byte-at-a-time parser.
 *
 * A UART, a USB CDC pipe, an RS-485 bus and a socket all do the same thing to a
 * message: they deliver it as bytes, with no idea where one message ends and the
 * next begins. This module turns that stream back into messages — framed,
 * length-checked, integrity-checked — and hands each one to the runtime's event
 * bus.
 *
 * COBS, not a start pattern or a timeout. Every frame is byte-stuffed so the
 * delimiter 0x00 appears only between frames, which lets the parser find a
 * boundary the instant it sees one, even if it joined the stream half way
 * through a frame or after a burst of noise. A protocol that resynchronises on
 * a timer is a protocol that stalls when the timer is wrong.
 *
 * No allocation. The receive buffer is in the instance, the frame is decoded
 * onto the stack, and the same functions run from an interrupt, a DMA callback
 * or a thread. `feed_byte` never blocks and never calls malloc.
 *
 * @project RoboHub Platform
 * @author Aurel Miron
 * @version 0.1
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INCLUDE_ROBOHUB_TRANSPORT_H
#define INCLUDE_ROBOHUB_TRANSPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#pragma once

#include "types.h"
#include "error.h"
#include "robohub/core/events.h"
#include "robohub/secure.h"

/* The resolved configuration, when a build wrote one; a host test has none. */
#if defined(__has_include)
#  if __has_include("autoconf.h")
#    include "autoconf.h"
#  endif
#endif

/** The largest COBS-encoded frame accepted or produced, in bytes. */
#ifndef RH_TRANSPORT_MAX_FRAME_SIZE
#  ifdef CONFIG_RH_TRANSPORT_MAX_FRAME_SIZE
#    define RH_TRANSPORT_MAX_FRAME_SIZE CONFIG_RH_TRANSPORT_MAX_FRAME_SIZE
#  else
#    define RH_TRANSPORT_MAX_FRAME_SIZE 256u
#  endif
#endif

/** The byte that separates frames; COBS guarantees it appears nowhere else. */
#define RH_TRANSPORT_DELIMITER 0x00u
/** msg_id, tx_id and payload_len, little-endian. */
#define RH_TRANSPORT_HEADER_SIZE 6u
/** The integrity check that follows the payload: Fletcher-16, little-endian. */
#define RH_TRANSPORT_CHECK_SIZE 2u
/**
 * @deprecated Use RH_TRANSPORT_CHECK_SIZE. The check is Fletcher-16, not a CRC;
 * the name is kept so existing code compiles and will be removed in a later
 * minor release.
 */
#define RH_TRANSPORT_CRC_SIZE RH_TRANSPORT_CHECK_SIZE
/** The smallest frame that carries a header and a check. */
#define RH_TRANSPORT_MIN_FRAME (RH_TRANSPORT_HEADER_SIZE + RH_TRANSPORT_CHECK_SIZE)

/* Marks a name kept only so existing code compiles. */
#if defined(__GNUC__) || defined(__clang__)
#  define RH_TRANSPORT_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
#  define RH_TRANSPORT_DEPRECATED(msg)
#endif

/* -------------------------------------------------------------------------- */
/*  Physical link                                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief The wire a transport is on.
 *
 * One call, and it is the platform's: writing bytes to a USB CDC endpoint, a
 * UART, an RS-485 transceiver or a socket. The transport neither knows nor asks
 * which, which is what lets one framing layer carry all of them.
 */
typedef struct RoboHubTransportPhy {
    /** Write bytes to the link; return 0 on success, -1 on failure. */
    int (*write)(const rh_u8* data, rh_size length, void* context);
    /** Whatever `write` needs to reach the link. */
    void* context;
} RoboHubTransportPhy;

/**
 * @brief Who takes a frame instead of the event bus.
 *
 * Called with every frame that checks out — on an authenticated link, after it
 * opened, with the inner message — before it would be published. Returning
 * non-zero takes the frame: it is not published. A relay sets one on the link
 * its boards are on, because what it does with a board's frame is carry it
 * whole (its id, its transaction and its payload, sealed or not) rather than
 * act on it, and a sealed id is not one the bus has a subscriber for.
 */
typedef int (*RoboHubTransportFrameHook)(rh_u16 msg_id, rh_u16 tx_id,
                                         const rh_u8* payload, rh_size length,
                                         void* user);

/* -------------------------------------------------------------------------- */
/*  Transport instance                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief One framed channel over one byte stream.
 *
 * A value the caller owns. The receive buffer is part of it, so the instance's
 * size is the maximum frame size and it belongs in static storage or on a
 * stack that can hold it — not in an interrupt's.
 */
typedef struct RoboHubTransport {
    RoboHubTransportPhy phy;
    rh_u8   rx[RH_TRANSPORT_MAX_FRAME_SIZE];
    rh_size rx_length;
    union {
        /** Frames whose Fletcher-16 check did not match. */
        rh_u32  check_errors;
        /**
         * @deprecated The same counter as check_errors, by its old name (the
         * check is Fletcher-16, not a CRC); kept so existing code compiles and
         * will be removed in a later minor release.
         */
        rh_u32  crc_errors RH_TRANSPORT_DEPRECATED("use check_errors: the check is Fletcher-16");
    };
    /** Frames delivered to the bus. */
    rh_u32  rx_packets;
    /** Frames handed to the link. */
    rh_u32  tx_packets;
    /** Frames dropped for being too long or malformed. */
    rh_u32  dropped;
    /**
     * The link's authentication, when it has any (robohub_transport_secure).
     * With it, every frame sent is sealed, every frame received must open, and
     * a frame that does not is dropped.
     */
    RoboHubSecure* secure;
    /** When the last HELLO was sent, so an unanswered one is sent again. */
    rh_u32  hello_ms;
    /** Who takes a frame before the bus does (robohub_transport_on_frame). */
    RoboHubTransportFrameHook on_frame;
    void*   on_frame_user;
    /* HEALTH (robohub_transport_report_health): whether this link reports the
     * board's, the device it names and the firmware, and whether it went out
     * this boot. */
    rh_bool health_on;
    rh_bool health_sent;
    rh_u16  health_area;
    rh_u16  health_node;
    rh_u16  health_firmware;
} RoboHubTransport;

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

/** Bind a transport to a link. The link must outlive the transport. */
RoboHubError robohub_transport_init(RoboHubTransport* transport,
                                    const RoboHubTransportPhy* phy);

/**
 * @brief Authenticate the link: from now on frames are sealed and opened with
 * `secure`, and a plain frame received is dropped. The first send starts the
 * session handshake. `secure` must outlive the transport.
 */
RoboHubError robohub_transport_secure(RoboHubTransport* transport, RoboHubSecure* secure);

/**
 * @brief Hand every frame that checks out to `hook` before the bus: a frame the
 * hook takes (returns non-zero for) is not published. NULL removes it.
 */
RoboHubError robohub_transport_on_frame(RoboHubTransport* transport,
                                        RoboHubTransportFrameHook hook, void* user);

/**
 * @brief Offer one byte from the link to the parser.
 *
 * Safe to call from an interrupt. When the byte completes a frame and the frame
 * checks out, the payload is published on the event bus before this returns,
 * with `msg_id` as the event and `tx_id` as the transaction.
 */
void robohub_transport_feed_byte(RoboHubTransport* transport, rh_u8 byte);

/** Offer a buffer of bytes to the parser. */
void robohub_transport_feed(RoboHubTransport* transport, const rh_u8* data, rh_size length);

/**
 * @brief Frame a payload and write it to the link.
 *
 * Returns `ROBOHUB_ERR_INVALID_ARG` when the payload cannot fit a frame. The
 * caller owns `payload`; it is read here and not kept. On an authenticated link
 * with no session yet, the frame is not sent: the handshake is started (or its
 * HELLO repeated) and `ROBOHUB_ERR_STATE` is returned, so a reading is never sent
 * unauthenticated.
 */
RoboHubError robohub_transport_send(RoboHubTransport* transport,
                                    robohub_event_id msg_id,
                                    rh_u16 tx_id,
                                    const void* payload,
                                    rh_u16 payload_len);

/**
 * @brief Open the session now, on an authenticated link that has none.
 *
 * Says HELLO, or says it again once `RH_TRANSPORT_HELLO_RETRY_MS` has passed
 * since the last, exactly as a send would — for a board that has nothing to send
 * yet (a node with no sensor, which still reports its health and takes updates)
 * and so would otherwise never start the handshake. Cheap to call every pass.
 * `ROBOHUB_OK` when the session is up, or the link is plain and needs none;
 * `ROBOHUB_ERR_STATE` while the handshake is under way.
 */
RoboHubError robohub_transport_connect(RoboHubTransport* transport);

/** The board's account of its last restart (robohub-protocol PROTO-0007). */
#define RH_TRANSPORT_MSG_HEALTH       0x0403u
/** A diagnostic request: answered, among whatever else answers it, with HEALTH. */
#define RH_TRANSPORT_MSG_DIAG_REQUEST 0x0401u
/** HEALTH's payload: version, device (version, area, node), reason, boot count,
 *  fault code, task, pc, uptime, firmware — little-endian. */
#define RH_TRANSPORT_HEALTH_SIZE      30u

/**
 * @brief Make this link report the board's health (RNODE-HEALTH-0004).
 *
 * From then on the link sends HEALTH — the reset reason, boot count and fault
 * record the node core read at boot (robohub/core/health.h), naming `area` and
 * `node` and carrying `firmware` — once per boot: on an authenticated link as
 * soon as its session with the gateway is established, on a plain link at once
 * (or, if that write fails, just before the next frame it sends). It sends it
 * again whenever a DIAG_REQUEST arrives. A session that drops and comes back does not send it again: it is
 * about the boot, and a diagnostic request asks anew.
 */
RoboHubError robohub_transport_report_health(RoboHubTransport* transport,
                                             rh_u16 area, rh_u16 node, rh_u16 firmware);

/**
 * @brief Send HEALTH now, as robohub_transport_report_health described it.
 *
 * ROBOHUB_ERR_STATE when the link does not report health, or an authenticated
 * link has no session yet.
 */
RoboHubError robohub_transport_send_health(RoboHubTransport* transport);

/**
 * @brief Write a HEALTH payload: the core's health, the device and firmware.
 *
 * `out` holds RH_TRANSPORT_HEALTH_SIZE bytes. The layout is robohub-protocol's
 * `Health` message, written here so the link needs no generated code.
 */
void robohub_transport_health_payload(rh_u16 area, rh_u16 node, rh_u16 firmware,
                                      rh_u8 out[RH_TRANSPORT_HEALTH_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ROBOHUB_TRANSPORT_H */
