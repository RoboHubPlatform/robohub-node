/**
 * @file managed.c
 * @brief The managed layer's update state machine (RNODE-UPD-0001…0006).
 *
 * See robohub/managed.h. The messages are laid out by hand from
 * robohub-protocol's schema (PROTO-0008), so the layer needs no generated code:
 *
 *   UpdateOffer    1 | manifest (57) | signature (64)
 *   UpdateManifest 1 | update_id u32 | uid[8] | firmware u16 | security u32 |
 *                  size u32 | sha256[32] | chunk_size u16
 *   UpdateStatus   1 | update_id u32 | state u32 | reason u32 | next u32 |
 *                  running_version u16 | running_slot u8
 *   UpdateCancel   1 | update_id u32
 *   UPDATE_CHUNK   update_id u32 | index u32 | data
 *
 * all little-endian.
 *
 * @project RoboHub Platform
 * @author Aurel Miron
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include "robohub/managed.h"
#include "robohub/secure.h"
#include "robohub/core/app.h"
#include "robohub/core/core.h"
#include "robohub/core/event.h"
#include "robohub/core/health.h"
#include "robohub/core/config.h"

#include "monocypher-ed25519.h"

#include <string.h>

#ifndef RH_UPDATE_CONFIRM_MS
#  ifdef CONFIG_RH_UPDATE_CONFIRM_MS
#    define RH_UPDATE_CONFIRM_MS CONFIG_RH_UPDATE_CONFIRM_MS
#  else
#    define RH_UPDATE_CONFIRM_MS 30000u
#  endif
#endif
#ifndef RH_UPDATE_TIMEOUT_MS
#  ifdef CONFIG_RH_UPDATE_TIMEOUT_MS
#    define RH_UPDATE_TIMEOUT_MS CONFIG_RH_UPDATE_TIMEOUT_MS
#  else
#    define RH_UPDATE_TIMEOUT_MS 60000u
#  endif
#endif
#ifndef RH_UPDATE_TRIAL_MS
#  ifdef CONFIG_RH_UPDATE_TRIAL_MS
#    define RH_UPDATE_TRIAL_MS CONFIG_RH_UPDATE_TRIAL_MS
#  else
#    define RH_UPDATE_TRIAL_MS 300000u
#  endif
#endif
#ifndef RH_UPDATE_MAX_CHUNK
#  ifdef CONFIG_RH_UPDATE_MAX_CHUNK
#    define RH_UPDATE_MAX_CHUNK CONFIG_RH_UPDATE_MAX_CHUNK
#  else
#    define RH_UPDATE_MAX_CHUNK 192u
#  endif
#endif

/* An acknowledgement at least every this many chunks: the window is four. */
#define RH_UPDATE_ACK_EVERY 4u

typedef struct {
    rh_u32 update_id;
    rh_u8  uid[8];
    rh_u16 firmware_version;
    rh_u32 security_version;
    rh_u32 size;
    rh_u8  sha256[32];
    rh_u16 chunk_size;
} manifest;

static RoboHubManagedConfig config;
static rh_bool configured;

static rh_bool receiving;
static manifest current;
static rh_u32 next_index;
static rh_u32 written;
static rh_u32 since_ack;
static rh_u32 last_chunk_ms;
static RoboHubSha256 hash;

static rh_bool outcome_reported;
static rh_u32 ended_id;         /* the last update reported as kept or rolled back */
static rh_update_state ended_state;
static rh_bool trial_seen;     /* the trial's start is known */
static rh_u32 trial_since;     /* when this boot's layer first saw the image pending */
static rh_u32 last_update_id;

/* -------------------------------------------------------------------------- */
/*  Wire                                                                      */
/* -------------------------------------------------------------------------- */

static rh_u32 get_u32(const rh_u8* p)
{
    return (rh_u32)p[0] | ((rh_u32)p[1] << 8) | ((rh_u32)p[2] << 16) | ((rh_u32)p[3] << 24);
}

static rh_u16 get_u16(const rh_u8* p)
{
    return (rh_u16)(p[0] | (p[1] << 8));
}

static void put_u32(rh_u8* p, rh_u32 v)
{
    p[0] = (rh_u8)v; p[1] = (rh_u8)(v >> 8); p[2] = (rh_u8)(v >> 16); p[3] = (rh_u8)(v >> 24);
}

static rh_u32 now_ms(void)
{
    return robohub_core_uptime_ms();
}

/* Statuses go out on the link; one that cannot (no session yet) is the next
 * poll's or the next frame's to repeat. */
// @req RNODE-UPD-0002, RNODE-UPD-0003
static RoboHubError send_status(rh_u32 update_id, rh_update_state state, rh_update_reason reason, rh_u32 next)
{
    rh_u8 out[RH_MANAGED_STATUS_SIZE];
    out[0] = 1u;
    put_u32(&out[1], update_id);
    put_u32(&out[5], (rh_u32)state);
    put_u32(&out[9], (rh_u32)reason);
    put_u32(&out[13], next);
    out[17] = (rh_u8)config.firmware_version;
    out[18] = (rh_u8)(config.firmware_version >> 8);
    int slot = rh_update_running_slot();
    out[19] = slot < 0 ? 0xFFu : (rh_u8)slot;
    return robohub_transport_send(config.transport, RH_MANAGED_MSG_STATUS, 0u, out, (rh_u16)sizeof(out));
}

static void report_outcome(void);
static rh_bool session_up(void);

/* -------------------------------------------------------------------------- */
/*  The offer                                                                 */
/* -------------------------------------------------------------------------- */

// @req RNODE-UPD-0002
int robohub_managed_check_signature(const rh_u8 key[32], const rh_u8 bytes[RH_MANAGED_MANIFEST_SIZE],
                                    const rh_u8 signature[64])
{
    static const char domain[] = "rh-update1";
    rh_u8 message[sizeof(domain) - 1u + RH_MANAGED_MANIFEST_SIZE];
    memcpy(message, domain, sizeof(domain) - 1u);
    memcpy(message + sizeof(domain) - 1u, bytes, RH_MANAGED_MANIFEST_SIZE);
    return crypto_ed25519_check(signature, key, message, sizeof(message));
}

static void parse_manifest(const rh_u8* m, manifest* out)
{
    out->update_id = get_u32(&m[1]);
    memcpy(out->uid, &m[5], 8u);
    out->firmware_version = get_u16(&m[13]);
    out->security_version = get_u32(&m[15]);
    out->size = get_u32(&m[19]);
    memcpy(out->sha256, &m[23], 32u);
    out->chunk_size = get_u16(&m[55]);
}

static void drop_transfer(void)
{
    receiving = RH_FALSE;
    next_index = 0u;
    written = 0u;
    since_ack = 0u;
}

// @req RNODE-UPD-0002
static RoboHubError on_offer(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    const rh_u8* p = (const rh_u8*)payload;
    if (size < RH_MANAGED_OFFER_SIZE || p[0] != 1u || p[1] != 1u) {
        rh_u32 uid = size >= 6u ? get_u32(&p[2]) : 0u;
        return send_status(uid, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_BAD_OFFER, 0u);
    }
    const rh_u8* bytes = &p[1];
    const rh_u8* signature = &p[1 + RH_MANAGED_MANIFEST_SIZE];
    manifest offer;
    parse_manifest(bytes, &offer);
    report_outcome();   /* an outcome not yet said is said, and known, first */

    /* The same update offered again while it is received: the link was lost,
     * and the gateway resumes where the board says. */
    if (receiving && offer.update_id == current.update_id && memcmp(offer.sha256, current.sha256, 32u) == 0 &&
        config.update_key != 0 && robohub_managed_check_signature(config.update_key, bytes, signature) == 0) {
        return send_status(offer.update_id, RH_UPDATE_STATE_ACCEPTED, RH_UPDATE_REASON_NONE, next_index);
    }

    rh_update_reason refusal = RH_UPDATE_REASON_NONE;
    rh_update_caps caps;
    rh_update_caps_get(&caps);
    if (config.update_key == 0) {
        refusal = RH_UPDATE_REASON_NOT_PROVISIONED;
    } else if (robohub_managed_check_signature(config.update_key, bytes, signature) != 0) {
        refusal = RH_UPDATE_REASON_SIGNATURE;
    } else if (config.uid == 0 || memcmp(offer.uid, config.uid, 8u) != 0) {
        refusal = RH_UPDATE_REASON_OTHER_BOARD;
    } else if (!caps.updatable) {
        refusal = RH_UPDATE_REASON_NOT_UPDATABLE;
    } else if (offer.security_version < rh_update_security_version()) {
        refusal = RH_UPDATE_REASON_DOWNGRADE;
    } else if (offer.size == 0u || offer.chunk_size == 0u || offer.chunk_size > RH_UPDATE_MAX_CHUNK) {
        refusal = RH_UPDATE_REASON_BAD_OFFER;
    } else if (offer.size > caps.slot_size) {
        refusal = RH_UPDATE_REASON_TOO_LARGE;
    } else if (receiving || rh_update_pending(0)) {
        refusal = RH_UPDATE_REASON_BUSY;
    }
    /* An update that has ended, offered again: the gateway did not hear how it
     * ended (the report went while the link was down), so it is told again
     * rather than started again (RNODE-UPD-0005). */
    if ((refusal == RH_UPDATE_REASON_NONE || refusal == RH_UPDATE_REASON_DOWNGRADE ||
         refusal == RH_UPDATE_REASON_BUSY) && ended_state != 0 && offer.update_id == ended_id) {
        return send_status(ended_id, ended_state, RH_UPDATE_REASON_NONE, 0u);
    }
    /* The update on trial, offered again: the gateway asks how it ends, and it
     * has not ended. It is verified and running once; the outcome is said when
     * it is kept or given up, not refused as another update would be. */
    rh_update_info trial;
    if (refusal == RH_UPDATE_REASON_BUSY && !receiving && rh_update_pending(&trial) &&
        trial.update_id == offer.update_id) {
        return send_status(offer.update_id, RH_UPDATE_STATE_VERIFIED, RH_UPDATE_REASON_NONE, 0u);
    }
    if (refusal != RH_UPDATE_REASON_NONE) {
        return send_status(offer.update_id, RH_UPDATE_STATE_REFUSED, refusal, 0u);
    }
    if (rh_update_begin(offer.size) != ROBOHUB_OK) {
        return send_status(offer.update_id, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_WRITE, 0u);
    }
    current = offer;
    drop_transfer();
    receiving = RH_TRUE;
    last_update_id = offer.update_id;
    last_chunk_ms = now_ms();
    robohub_sha256_init(&hash);
    return send_status(offer.update_id, RH_UPDATE_STATE_ACCEPTED, RH_UPDATE_REASON_NONE, 0u);
}

/* -------------------------------------------------------------------------- */
/*  The chunks                                                                */
/* -------------------------------------------------------------------------- */

static void fail(rh_update_reason reason)
{
    rh_u32 id = current.update_id;
    drop_transfer();
    (void)send_status(id, RH_UPDATE_STATE_FAILED, reason, 0u);
}

// @req RNODE-UPD-0004
static void finish(void)
{
    rh_u8 digest[32];
    robohub_sha256_final(&hash, digest);
    if (memcmp(digest, current.sha256, 32u) != 0) {
        fail(RH_UPDATE_REASON_HASH);
        return;
    }
    (void)send_status(current.update_id, RH_UPDATE_STATE_VERIFIED, RH_UPDATE_REASON_NONE, next_index);
    rh_update_info info;
    info.update_id = current.update_id;
    info.firmware_version = current.firmware_version;
    info.security_version = current.security_version;
    drop_transfer();
    /* Returns only when the platform could not: the board keeps what it runs. */
    if (rh_update_boot_once(&info) != ROBOHUB_OK) {
        (void)send_status(info.update_id, RH_UPDATE_STATE_FAILED, RH_UPDATE_REASON_WRITE, 0u);
    }
}

// @req RNODE-UPD-0003
static RoboHubError on_chunk(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    const rh_u8* p = (const rh_u8*)payload;
    if (!receiving || size < RH_MANAGED_CHUNK_HEADER || get_u32(&p[0]) != current.update_id) {
        return ROBOHUB_OK; /* a chunk of nothing under way: dropped */
    }
    rh_u32 index = get_u32(&p[4]);
    rh_u32 length = (rh_u32)size - RH_MANAGED_CHUNK_HEADER;
    rh_u32 offset = index * current.chunk_size;
    rh_u32 expected = current.size - written;
    if (expected > current.chunk_size) {
        expected = current.chunk_size;
    }
    if (index != next_index || length != expected) {
        /* Out of order, or not the length it must be: dropped, and the board
         * says where it is so the sender goes back there. */
        since_ack = 0u;
        return send_status(current.update_id, RH_UPDATE_STATE_RECEIVING, RH_UPDATE_REASON_NONE, next_index);
    }
    if (rh_update_write(offset, &p[RH_MANAGED_CHUNK_HEADER], length) != ROBOHUB_OK) {
        fail(RH_UPDATE_REASON_WRITE);
        return ROBOHUB_OK;
    }
    robohub_sha256_update(&hash, &p[RH_MANAGED_CHUNK_HEADER], length);
    written += length;
    next_index++;
    last_chunk_ms = now_ms();
    if (written == current.size) {
        finish();
        return ROBOHUB_OK;
    }
    if (++since_ack >= RH_UPDATE_ACK_EVERY) {
        since_ack = 0u;
        return send_status(current.update_id, RH_UPDATE_STATE_RECEIVING, RH_UPDATE_REASON_NONE, next_index);
    }
    return ROBOHUB_OK;
}

// @req RNODE-UPD-0006
static RoboHubError on_cancel(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    const rh_u8* p = (const rh_u8*)payload;
    if (receiving && size >= RH_MANAGED_CANCEL_SIZE && get_u32(&p[1]) == current.update_id) {
        fail(RH_UPDATE_REASON_CANCELLED);
    }
    return ROBOHUB_OK;
}

/* -------------------------------------------------------------------------- */
/*  Every second: the timeout, the confirmation, the report                   */
/* -------------------------------------------------------------------------- */

static rh_bool session_up(void)
{
    RoboHubSecure* secure = config.transport != 0 ? config.transport->secure : 0;
    return secure == 0 || robohub_secure_ready(secure);
}

/* The last update's outcome — kept or rolled back — taken from the platform and
 * said once the session is up; afterwards an offer of that update is answered
 * with it (RNODE-UPD-0005). */
static void report_outcome(void)
{
    if (outcome_reported || !session_up() || rh_update_pending(0)) {
        return;
    }
    rh_update_info done;
    rh_update_outcome outcome = rh_update_take_outcome(&done);
    if (outcome == RH_UPDATE_KEPT) {
        ended_state = RH_UPDATE_STATE_CONFIRMED;
    } else if (outcome == RH_UPDATE_ROLLED_BACK) {
        ended_state = RH_UPDATE_STATE_ROLLED_BACK;
    }
    if (outcome != RH_UPDATE_NONE) {
        ended_id = done.update_id;
        last_update_id = done.update_id;
        (void)send_status(ended_id, ended_state, RH_UPDATE_REASON_NONE, 0u);
    }
    outcome_reported = RH_TRUE;
}

// @req RNODE-UPD-0005, RNODE-UPD-0006, RNODE-UPD-0008
void robohub_managed_poll(rh_u32 now)
{
    if (!configured) {
        return;
    }
    if (receiving && (rh_u32)(now - last_chunk_ms) >= RH_UPDATE_TIMEOUT_MS) {
        fail(RH_UPDATE_REASON_TIMEOUT);
    }
    rh_update_info info;
    if (rh_update_pending(&info)) {
        robohub_core_health health;
        robohub_core_health_get(&health);
        if (session_up() && health.on_time && (rh_u32)(now - health.on_time_since_ms) >= (config.confirm_ms != 0u ? config.confirm_ms : RH_UPDATE_CONFIRM_MS) &&
            rh_update_confirm() == ROBOHUB_OK) {
            /* Said now, from the platform's outcome, so no offer in between
             * finds it neither pending nor ended. */
            last_update_id = info.update_id;
            report_outcome();
            return;
        }
        /* Never confirmed within the limit — its session never came up: given
         * up, and the board restarts into the image it ran before
         * (RNODE-UPD-0008). Counted from when this boot's layer first saw it
         * pending: a clock may count from the machine's start, not this boot's. */
        if (!trial_seen) {
            trial_seen = RH_TRUE;
            trial_since = now;
        }
        if ((rh_u32)(now - trial_since) >= (config.trial_ms != 0u ? config.trial_ms : RH_UPDATE_TRIAL_MS)) {
            (void)rh_update_reject();
        }
        return;
    }
    report_outcome();
}

static RoboHubError managed_task(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    /* A confirmation taken this second is reported the next. */
    robohub_managed_poll(now_ms());
    return ROBOHUB_OK;
}

static RoboHubError managed_start(void)
{
    (void)robohub_core_event_subscribe((robohub_event_id)RH_MANAGED_MSG_OFFER, on_offer, 0);
    (void)robohub_core_event_subscribe((robohub_event_id)RH_MANAGED_MSG_CHUNK, on_chunk, 0);
    (void)robohub_core_event_subscribe((robohub_event_id)RH_MANAGED_MSG_CANCEL, on_cancel, 0);
    robohub_core_task_id task = RH_CORE_TASK_INVALID;
    return robohub_core_task_add("managed", managed_task, 0, 1000u, &task);
}

/* At the core's init, right after its event bus starts: an offer that arrives
 * before the loop runs is heard. */
static const robohub_core_app managed_app = {
    .name = "managed",
    .init = managed_start,
};

static rh_bool registered;

// @req RNODE-UPD-0001
RoboHubError robohub_managed_init(const RoboHubManagedConfig* given)
{
    if (given == 0 || given->transport == 0) {
        return ROBOHUB_ERR_INVALID_ARG;
    }
    config = *given;
    configured = RH_TRUE;
    drop_transfer();
    outcome_reported = RH_FALSE;
    trial_seen = RH_FALSE;
    ended_id = 0u;
    ended_state = (rh_update_state)0;
    if (registered) {
        return ROBOHUB_OK; /* given again: the new configuration, one application */
    }
    registered = RH_TRUE;
    return robohub_core_app_register(&managed_app);
}

void robohub_managed_state(RoboHubManagedState* out)
{
    if (out == 0) {
        return;
    }
    out->receiving = receiving;
    out->update_id = receiving ? current.update_id : last_update_id;
    out->next_index = next_index;
    out->size = current.size;
    out->pending = rh_update_pending(0);
    out->reported = outcome_reported;
}
