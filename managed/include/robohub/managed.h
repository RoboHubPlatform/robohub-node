/**
 * @file managed.h
 * @brief The managed layer: firmware updates over the link (RNODE-UPD-0001…0007).
 *
 * A board that chooses it (`CONFIG_RH_NODE_MANAGED`) takes an update from its
 * gateway (robohub ADR-0022, this collection's ADR-0003):
 *
 *   UPDATE_OFFER   the manifest is checked — its Ed25519 signature under the
 *                  mesh's update key, this board's uid, no downgrade, it fits —
 *                  and answered accepted (next index 0) or refused with a reason
 *   UPDATE_CHUNK   written in order to the inactive slot and hashed; the next
 *                  index expected is answered every fourth chunk and on a chunk
 *                  out of order; an offer of the same update again (a lost link)
 *                  is answered with the index to resume at
 *   last chunk     the SHA-256 is compared, `verified` answered, the slot booted
 *                  once (robohub/update_slot.h)
 *   next boot      confirmed once the link's session is up and every task has
 *                  kept its time for 30 s; `confirmed` or `rolled back` reported
 *   UPDATE_CANCEL  or 60 s without a chunk: the transfer is dropped, `failed`
 *
 * One update at a time, its state in RAM: a reset during a transfer drops it.
 *
 * @project RoboHub Platform
 * @author Aurel Miron
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INCLUDE_ROBOHUB_MANAGED_H
#define INCLUDE_ROBOHUB_MANAGED_H

#include "types.h"
#include "error.h"
#include "robohub/transport.h"
#include "robohub/update_slot.h"
#include "robohub/core/scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The update messages (robohub-protocol PROTO-0008). */
#define RH_MANAGED_MSG_OFFER  0x0501u
#define RH_MANAGED_MSG_CHUNK  0x0502u
#define RH_MANAGED_MSG_STATUS 0x0503u
#define RH_MANAGED_MSG_CANCEL 0x0504u

/* Wire sizes: an offer is its version, the manifest (version and 56 bytes) and
 * the signature; a status its version and 19 bytes; a cancel its version and
 * the id; a chunk's header the id and the index. */
#define RH_MANAGED_MANIFEST_SIZE 57u
#define RH_MANAGED_OFFER_SIZE    122u
#define RH_MANAGED_STATUS_SIZE   20u
#define RH_MANAGED_CANCEL_SIZE   5u
#define RH_MANAGED_CHUNK_HEADER  8u

/* What the board says (UpdateState) and why (UpdateReason). */
typedef enum {
    RH_UPDATE_STATE_ACCEPTED    = 1,
    RH_UPDATE_STATE_REFUSED     = 2,
    RH_UPDATE_STATE_RECEIVING   = 3,
    RH_UPDATE_STATE_VERIFIED    = 4,
    RH_UPDATE_STATE_CONFIRMED   = 5,
    RH_UPDATE_STATE_ROLLED_BACK = 6,
    RH_UPDATE_STATE_FAILED      = 7,
} rh_update_state;

typedef enum {
    RH_UPDATE_REASON_NONE            = 0,
    RH_UPDATE_REASON_SIGNATURE       = 1,
    RH_UPDATE_REASON_OTHER_BOARD     = 2,
    RH_UPDATE_REASON_DOWNGRADE       = 3,
    RH_UPDATE_REASON_TOO_LARGE       = 4,
    RH_UPDATE_REASON_NOT_UPDATABLE   = 5,
    RH_UPDATE_REASON_BUSY            = 6,
    RH_UPDATE_REASON_NOT_PROVISIONED = 7,
    RH_UPDATE_REASON_HASH            = 8,
    RH_UPDATE_REASON_CANCELLED       = 9,
    RH_UPDATE_REASON_TIMEOUT         = 10,
    RH_UPDATE_REASON_WRITE           = 11,
    RH_UPDATE_REASON_BAD_OFFER       = 12,
} rh_update_reason;

/** What the layer is given. */
typedef struct {
    /** The link to the gateway: where offers come from and statuses go. */
    RoboHubTransport* transport;
    /** The board's uid (RH_SECURE_UID_SIZE bytes): a manifest must name it. */
    const rh_u8* uid;
    /** The mesh's Ed25519 update public key (32 bytes), from the build's
     *  robohub_update_key.h; NULL refuses every offer as not provisioned. */
    const rh_u8* update_key;
    /** The firmware running, as HELLO and HEALTH say it. */
    rh_u16 firmware_version;
    /** How long an image on trial must be healthy before it is kept, in ms;
     *  0 for RH_UPDATE_CONFIRM_MS (30 s). */
    rh_u32 confirm_ms;
} RoboHubManagedConfig;

/** Where the layer stands, for a test or a diagnostic page. */
typedef struct {
    rh_bool receiving;       /**< a transfer is under way */
    rh_u32 update_id;        /**< the one under way, or the last */
    rh_u32 next_index;       /**< the next chunk it expects */
    rh_u32 size;             /**< the image's size */
    rh_bool pending;         /**< this image was booted once and is not confirmed */
    rh_bool reported;        /**< the last update's outcome was reported */
} RoboHubManagedState;

/**
 * Set the layer up: registers an application with the core whose start
 * subscribes to the update messages and adds the layer's task. Call before
 * robohub_core_init, like any application registration. One per program.
 */
RoboHubError robohub_managed_init(const RoboHubManagedConfig* config);

/** The layer's state. */
void robohub_managed_state(RoboHubManagedState* out);

/**
 * The layer's periodic work, which its task runs every second: the transfer's
 * timeout, the confirmation of an image booted once, and the report of the last
 * update's outcome. A test may call it with its own clock.
 */
void robohub_managed_poll(rh_u32 now_ms);

/**
 * Check an Ed25519 signature over "rh-update1" followed by a manifest's wire
 * bytes (RH_MANAGED_MANIFEST_SIZE). 0 when it verifies.
 */
int robohub_managed_check_signature(const rh_u8 key[32], const rh_u8 manifest[RH_MANAGED_MANIFEST_SIZE],
                                    const rh_u8 signature[64]);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ROBOHUB_MANAGED_H */
