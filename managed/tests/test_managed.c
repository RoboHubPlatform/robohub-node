/*
 * test_managed.c — the managed layer's update state machine on a fake board
 * (RNODE-UPD-0001…0007).
 *
 * The board is the real core, link and managed layer over a fake update slot
 * (an in-memory image, strong over robohub-common's weak defaults) and a fake
 * clock. The gateway is the other end of a plain link: it sends offers, chunks
 * and cancels with its own transport, and reads the board's UPDATE_STATUS off the
 * event bus. Manifests are signed here with the vendored Monocypher.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "robohub/managed.h"
#include "robohub/secure.h"
#include "robohub/transport.h"
#include "robohub/update_slot.h"
#include "robohub/core/core.h"
#include "robohub/core/event.h"
#include "robohub/core/health.h"
#include "core_internal.h"
#include "monocypher-ed25519.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  The fake board: a clock and an update slot                                */
/* -------------------------------------------------------------------------- */

static rh_u32 clock_ms = 1000u;
static rh_millis fake_millis(RoboHubTimer* t) { (void)t; return clock_ms; }
static void fake_delay(RoboHubTimer* t, rh_u32 ms) { (void)t; clock_ms += ms; }
static RoboHubTimer fake_timer = { .name = "system_timer", .get_millis = fake_millis, .delay_ms = fake_delay };

#define SLOT_SIZE 4096u
static bool slot_updatable = true;
static uint8_t slot[SLOT_SIZE];
static uint32_t slot_begun = 0u, slot_written = 0u;
static int boots;
static rh_update_info booted;
static bool slot_pending;
static rh_update_info pending_info;
static rh_update_outcome slot_outcome = RH_UPDATE_NONE;
static rh_update_info outcome_info;
static uint32_t slot_security = 5u;
static int confirms;

void rh_update_caps_get(rh_update_caps* caps)
{
    caps->updatable = slot_updatable;
    caps->slot_size = slot_updatable ? SLOT_SIZE : 0u;
}
RoboHubError rh_update_begin(uint32_t size)
{
    if (size > SLOT_SIZE) return ROBOHUB_ERR_NO_MEM;
    slot_begun = size; slot_written = 0u;
    return ROBOHUB_OK;
}
RoboHubError rh_update_write(uint32_t offset, const uint8_t* data, uint32_t length)
{
    if (offset != slot_written || length > slot_begun - slot_written) return ROBOHUB_ERR_STATE;
    memcpy(slot + offset, data, length);
    slot_written += length;
    return ROBOHUB_OK;
}
RoboHubError rh_update_read(uint32_t offset, uint8_t* out, uint32_t length)
{
    memcpy(out, slot + offset, length);
    return ROBOHUB_OK;
}
RoboHubError rh_update_boot_once(const rh_update_info* info)
{
    if (slot_written != slot_begun) return ROBOHUB_ERR_STATE;
    boots++;
    booted = *info;
    return ROBOHUB_OK; /* a real board restarts here */
}
bool rh_update_pending(rh_update_info* out)
{
    if (slot_pending && out) *out = pending_info;
    return slot_pending;
}
RoboHubError rh_update_confirm(void)
{
    if (!slot_pending) return ROBOHUB_ERR_STATE;
    confirms++;
    slot_pending = false;
    if (pending_info.security_version > slot_security) slot_security = pending_info.security_version;
    slot_outcome = RH_UPDATE_KEPT;
    outcome_info = pending_info;
    return ROBOHUB_OK;
}
rh_update_outcome rh_update_take_outcome(rh_update_info* out)
{
    rh_update_outcome o = slot_outcome;
    if (o != RH_UPDATE_NONE && out) *out = outcome_info;
    slot_outcome = RH_UPDATE_NONE;
    return o;
}
uint32_t rh_update_security_version(void) { return slot_security; }
int rh_update_running_slot(void) { return 0; }

/* -------------------------------------------------------------------------- */
/*  The gateway end                                                           */
/* -------------------------------------------------------------------------- */

static RoboHubTransport gw, board;
static int forward(const rh_u8* data, rh_size length, void* context)
{
    robohub_transport_feed((RoboHubTransport*)context, data, length);
    return 0;
}

typedef struct { rh_u32 id, state, reason, next; rh_u16 version; } status;
static status last;
static int statuses;
static RoboHubError on_status(robohub_event_id id, const void* payload, rh_size size, void* user)
{
    (void)id; (void)user;
    const rh_u8* p = payload;
    assert(size == RH_MANAGED_STATUS_SIZE && p[0] == 1u);
#define U32(o) ((rh_u32)p[o] | ((rh_u32)p[o + 1] << 8) | ((rh_u32)p[o + 2] << 16) | ((rh_u32)p[o + 3] << 24))
    last.id = U32(1); last.state = U32(5); last.reason = U32(9); last.next = U32(13);
    last.version = (rh_u16)(p[17] | (p[18] << 8));
    statuses++;
    return ROBOHUB_OK;
}

static const rh_u8 board_uid[8] = { 0xB0, 1, 2, 3, 4, 5, 6, 7 };
static rh_u8 secret[64], public_key[32], other_secret[64], other_public[32];
static uint8_t image[1000];

static void put32(rh_u8* p, rh_u32 v) { p[0] = (rh_u8)v; p[1] = (rh_u8)(v >> 8); p[2] = (rh_u8)(v >> 16); p[3] = (rh_u8)(v >> 24); }

static void sha256(const uint8_t* data, rh_size n, rh_u8 out[32])
{
    RoboHubSha256 s;
    robohub_sha256_init(&s);
    robohub_sha256_update(&s, data, n);
    robohub_sha256_final(&s, out);
}

/* An offer for `image`, signed by `key`, with the fields a case changes. */
static void offer(rh_u32 id, const rh_u8* uid, rh_u32 security, rh_u32 size, rh_u16 chunk,
                  const rh_u8 digest[32], const rh_u8* key_secret, rh_size length)
{
    rh_u8 o[RH_MANAGED_OFFER_SIZE];
    rh_u8* m = &o[1];
    o[0] = 1u; m[0] = 1u;
    put32(&m[1], id);
    memcpy(&m[5], uid, 8);
    m[13] = 9u; m[14] = 0u;                /* firmware 9 */
    put32(&m[15], security);
    put32(&m[19], size);
    memcpy(&m[23], digest, 32);
    m[55] = (rh_u8)chunk; m[56] = (rh_u8)(chunk >> 8);
    rh_u8 message[10 + RH_MANAGED_MANIFEST_SIZE];
    memcpy(message, "rh-update1", 10);
    memcpy(message + 10, m, RH_MANAGED_MANIFEST_SIZE);
    crypto_ed25519_sign(&o[1 + RH_MANAGED_MANIFEST_SIZE], key_secret, message, sizeof message);
    assert(robohub_transport_send(&gw, RH_MANAGED_MSG_OFFER, 0u, o, (rh_u16)length) == ROBOHUB_OK);
}

static void good_offer(rh_u32 id, const rh_u8 digest[32])
{
    offer(id, board_uid, 5u, sizeof image, 192u, digest, secret, RH_MANAGED_OFFER_SIZE);
}

static void chunk(rh_u32 id, rh_u32 index)
{
    rh_u8 c[RH_MANAGED_CHUNK_HEADER + 192];
    put32(&c[0], id);
    put32(&c[4], index);
    rh_u32 at = index * 192u;
    rh_u32 n = sizeof image - at < 192u ? (rh_u32)sizeof image - at : 192u;
    memcpy(&c[8], image + at, n);
    assert(robohub_transport_send(&gw, RH_MANAGED_MSG_CHUNK, 0u, c, (rh_u16)(8u + n)) == ROBOHUB_OK);
}

static void cancel(rh_u32 id)
{
    rh_u8 c[RH_MANAGED_CANCEL_SIZE] = { 1u };
    put32(&c[1], id);
    assert(robohub_transport_send(&gw, RH_MANAGED_MSG_CANCEL, 0u, c, (rh_u16)sizeof c) == ROBOHUB_OK);
}

static void expect(rh_u32 id, rh_u32 state, rh_u32 reason, rh_u32 next, const char* what)
{
    if (last.id != id || last.state != state || last.reason != reason || last.next != next) {
        fprintf(stderr, "FAIL %s: got id %u state %u reason %u next %u\n", what,
                (unsigned)last.id, (unsigned)last.state, (unsigned)last.reason, (unsigned)last.next);
        assert(0);
    }
}

/* -------------------------------------------------------------------------- */
/*  RFC 8032, test 1: the vendored verifier is Ed25519                        */
/* -------------------------------------------------------------------------- */

static void hex(const char* s, rh_u8* out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (rh_u8)v;
    }
}

static void rfc8032(void)
{
    rh_u8 seed[32], pub[32], want_pub[32], sig[64], sk[64], derived[32];
    hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
    hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", want_pub, 32);
    hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", sig, 64);
    memcpy(pub, want_pub, 32);
    assert(crypto_ed25519_check(sig, pub, (const rh_u8*)"", 0) == 0);
    sig[0] ^= 1u;
    assert(crypto_ed25519_check(sig, pub, (const rh_u8*)"", 0) != 0);
    crypto_ed25519_key_pair(sk, derived, seed);
    assert(memcmp(derived, want_pub, 32) == 0);
}

// @verifies RNODE-UPD-0001, RNODE-UPD-0002, RNODE-UPD-0003, RNODE-UPD-0004, RNODE-UPD-0005, RNODE-UPD-0006, RNODE-UPD-0007
int main(void)
{
    rfc8032();

    rh_u8 seed[32];
    for (int i = 0; i < 32; i++) seed[i] = (rh_u8)(i + 1);
    crypto_ed25519_key_pair(secret, public_key, seed);
    for (int i = 0; i < 32; i++) seed[i] = (rh_u8)(0x80 + i);
    crypto_ed25519_key_pair(other_secret, other_public, seed);
    for (size_t i = 0; i < sizeof image; i++) image[i] = (uint8_t)(i * 7u + (i >> 3));
    rh_u8 digest[32], wrong[32];
    sha256(image, sizeof image, digest);
    memcpy(wrong, digest, 32); wrong[0] ^= 1u;

    const RoboHubTransportPhy to_board = { .write = forward, .context = &board };
    const RoboHubTransportPhy to_gw = { .write = forward, .context = &gw };
    assert(robohub_transport_init(&gw, &to_board) == ROBOHUB_OK);
    assert(robohub_transport_init(&board, &to_gw) == ROBOHUB_OK);

    /* Not provisioned: no key in the build. */
    RoboHubManagedConfig config = { .transport = &board, .uid = board_uid, .update_key = 0, .firmware_version = 3u };
    assert(robohub_managed_init(&config) == ROBOHUB_OK);
    robohub_core_config core = { .timer = &fake_timer };
    assert(robohub_core_init(&core) == ROBOHUB_OK);   /* the layer subscribes here */
    assert(robohub_core_event_subscribe((robohub_event_id)RH_MANAGED_MSG_STATUS, on_status, 0) == ROBOHUB_OK);
    good_offer(1u, digest);
    expect(1u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_NOT_PROVISIONED, 0u, "no key");
    assert(last.version == 3u);

    config.update_key = public_key;
    assert(robohub_managed_init(&config) == ROBOHUB_OK);

    /* Every refusal (RNODE-UPD-0002). */
    offer(2u, board_uid, 5u, sizeof image, 192u, digest, other_secret, RH_MANAGED_OFFER_SIZE);
    expect(2u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_SIGNATURE, 0u, "another key");
    const rh_u8 other_uid[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };
    offer(3u, other_uid, 5u, sizeof image, 192u, digest, secret, RH_MANAGED_OFFER_SIZE);
    expect(3u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_OTHER_BOARD, 0u, "another board");
    offer(4u, board_uid, 4u, sizeof image, 192u, digest, secret, RH_MANAGED_OFFER_SIZE);
    expect(4u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_DOWNGRADE, 0u, "a downgrade");
    offer(5u, board_uid, 5u, SLOT_SIZE + 1u, 192u, digest, secret, RH_MANAGED_OFFER_SIZE);
    expect(5u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_TOO_LARGE, 0u, "too large");
    offer(6u, board_uid, 5u, sizeof image, 0u, digest, secret, RH_MANAGED_OFFER_SIZE);
    expect(6u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_BAD_OFFER, 0u, "no chunk size");
    offer(7u, board_uid, 5u, sizeof image, 192u, digest, secret, 60u);
    expect(7u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_BAD_OFFER, 0u, "a short offer");
    slot_updatable = false;
    good_offer(8u, digest);
    expect(8u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_NOT_UPDATABLE, 0u, "no second image");
    slot_updatable = true;

    /* Accepted; four chunks, an acknowledgement (RNODE-UPD-0003). */
    good_offer(10u, digest);
    expect(10u, RH_UPDATE_STATE_ACCEPTED, RH_UPDATE_REASON_NONE, 0u, "accepted");
    good_offer(11u, digest);
    expect(11u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_BUSY, 0u, "another while one runs");
    int before = statuses;
    chunk(10u, 0); chunk(10u, 1); chunk(10u, 2);
    assert(statuses == before);                        /* no answer inside the window */
    chunk(10u, 3);
    expect(10u, RH_UPDATE_STATE_RECEIVING, RH_UPDATE_REASON_NONE, 4u, "every fourth chunk");
    chunk(10u, 5);                                     /* chunk 4 was lost */
    expect(10u, RH_UPDATE_STATE_RECEIVING, RH_UPDATE_REASON_NONE, 4u, "out of order: where it is");
    /* The link was lost: the same offer again resumes at 4. */
    good_offer(10u, digest);
    expect(10u, RH_UPDATE_STATE_ACCEPTED, RH_UPDATE_REASON_NONE, 4u, "resume");
    chunk(10u, 4);
    assert(boots == 0);
    chunk(10u, 5);
    expect(10u, RH_UPDATE_STATE_VERIFIED, RH_UPDATE_REASON_NONE, 6u, "verified");
    assert(boots == 1 && booted.update_id == 10u && booted.firmware_version == 9u && booted.security_version == 5u);
    assert(memcmp(slot, image, sizeof image) == 0);

    /* A hash that does not match: nothing is booted (RNODE-UPD-0004). */
    good_offer(12u, wrong);
    for (rh_u32 i = 0; i < 6; i++) chunk(12u, i);
    expect(12u, RH_UPDATE_STATE_FAILED, RH_UPDATE_REASON_HASH, 0u, "a bad hash");
    assert(boots == 1);

    /* Cancelled, and a stray chunk after it (RNODE-UPD-0006). */
    good_offer(13u, digest);
    chunk(13u, 0);
    cancel(13u);
    expect(13u, RH_UPDATE_STATE_FAILED, RH_UPDATE_REASON_CANCELLED, 0u, "cancelled");
    before = statuses;
    chunk(13u, 1);
    assert(statuses == before);

    /* Silence: dropped after a minute. */
    good_offer(14u, digest);
    robohub_managed_poll(clock_ms + 59000u);
    expect(14u, RH_UPDATE_STATE_ACCEPTED, RH_UPDATE_REASON_NONE, 0u, "still waiting at 59 s");
    robohub_managed_poll(clock_ms + 60000u);
    expect(14u, RH_UPDATE_STATE_FAILED, RH_UPDATE_REASON_TIMEOUT, 0u, "dropped at 60 s");

    /* The next boot: pending, kept only after 30 s on time (RNODE-UPD-0005). */
    slot_pending = true;
    pending_info = (rh_update_info){ 77u, 9u, 8u };
    assert(robohub_managed_init(&config) == ROBOHUB_OK); /* a new boot's layer */
    good_offer(15u, digest);
    expect(15u, RH_UPDATE_STATE_REFUSED, RH_UPDATE_REASON_BUSY, 0u, "nothing while one is on trial");
    robohub_core_watchdog_service(clock_ms);           /* every task on time from now */
    robohub_core_health h;
    robohub_core_health_get(&h);
    assert(h.on_time);
    rh_u32 since = h.on_time_since_ms;
    robohub_managed_poll(since + 29999u);
    assert(confirms == 0);
    robohub_managed_poll(since + 30000u);
    assert(confirms == 1 && slot_security == 8u);
    before = statuses;
    robohub_managed_poll(since + 31000u);
    expect(77u, RH_UPDATE_STATE_CONFIRMED, RH_UPDATE_REASON_NONE, 0u, "confirmed");
    robohub_managed_poll(since + 32000u);
    assert(statuses == before + 1);                    /* said once */

    /* A boot after an image that was not kept: rolled back, said once. */
    slot_outcome = RH_UPDATE_ROLLED_BACK;
    outcome_info = (rh_update_info){ 78u, 10u, 9u };
    assert(robohub_managed_init(&config) == ROBOHUB_OK);
    robohub_managed_poll(clock_ms);
    expect(78u, RH_UPDATE_STATE_ROLLED_BACK, RH_UPDATE_REASON_NONE, 0u, "rolled back");
    before = statuses;
    robohub_managed_poll(clock_ms + 1000u);
    assert(statuses == before);

    printf("managed layer: ok\n");
    return 0;
}
