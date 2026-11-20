/**
 * @file secure.h
 * @brief Authenticated frames between a board and its gateway.
 *
 * A frame on a serial line, a UDP socket or an uplink is read by whoever is on
 * that wire, and anything on it can write one. This layer makes every frame a
 * board sends or accepts one its gateway vouched for: authentic, unmodified,
 * fresh, and never replayed. It does not encrypt; readings and commands are not
 * secret, but they must not be forged.
 *
 * Keys. The board carries a 64-bit id and a 128-bit device key, written into its
 * firmware when it is built. The gateway derives the same key from its own key
 * and the id, so no key crosses a network:
 *
 *     K_dev = HMAC-SHA256(K_gw, "rh-dev1" | uid)[0:16]
 *
 * Session. The board says hello with a fresh nonce; the gateway answers with a
 * session id, its own nonce and a lifetime, bound to the board's nonce so an old
 * answer cannot be replayed. Each direction then has its own key:
 *
 *     HELLO    uid(8) | nonce_b(8) | tag(8)                        tag over "rh-hello1" | uid | nonce_b
 *     SESSION  uid(8) | sid(2) | nonce_g(8) | lifetime_s(2) | tag  tag over "rh-session1" | uid | nonce_b | sid | nonce_g | lifetime
 *     K_up   = HMAC-SHA256(K_dev, "rh-up1"   | nonce_b | nonce_g)[0:16]
 *     K_down = HMAC-SHA256(K_dev, "rh-down1" | nonce_b | nonce_g)[0:16]
 *
 * Frame. A secured frame keeps the framing (COBS, the header, the check) and sets
 * the message id's high bit. Its payload is
 *
 *     sid(2) | counter(4) | payload | tag(8)
 *     tag = HMAC-SHA256(K_dir, msg_id | tx_id | sid | counter | payload)[0:8]
 *
 * and a receiver drops it unless the session is current, the counter is greater
 * than the last one accepted in that direction, and the tag matches.
 *
 * Key epochs. A board's key can be rotated without reflashing it. Epoch 0 is the
 * key above; epoch n >= 1 is
 *
 *     K_dev(n) = HMAC-SHA256(K_gw, "rh-dev2" | uid | n as u32 LE)[0:16]
 *
 * Nothing in the hello says which epoch a board holds: the gateway tries the one
 * its node manifest names for the board, then the one before it while the board
 * has not confirmed the new one. When a board opens a session under an older
 * epoch than the manifest's, the gateway sends, sealed under that session,
 *
 *     REKEY      new_epoch(4) | wrapped(16)
 *                wrapped = K_dev(new) XOR HMAC-SHA256(K_dev, "rh-wrap1" | sid | ctr)[0:16]
 *
 * with sid and ctr the REKEY frame's own session id and counter. The board
 * unwraps the key, stores it (robohub/keystore.h, or the instance's own store
 * hook), and answers, sealed under the same session,
 *
 *     REKEY_ACK  new_epoch(4) | proof(8)
 *                proof = HMAC-SHA256(K_dev(new), "rh-rekey-ok" | uid | new_epoch)[0:8]
 *
 * It keeps its current key for the rest of the session and says its next hello
 * with the new one. A board that cannot store the key answers epoch 0 and a zero
 * proof, which the gateway reports as "cannot rotate". At init the stored key is
 * used instead of the compiled-in one when its epoch is newer and its scope is
 * the build's.
 *
 * Scope. A stored key is kept with the build's key scope (RH_KEY_SCOPE in the
 * robohub_secret.h Mesh writes: HMAC-SHA256(K_gw, "rh-scope1")[0:8], the same
 * for every board of one gateway). A key stored under another scope — a board
 * moved to another mesh and flashed for it — is ignored, and the compiled-in key
 * is used. A build without a scope (a Linux host node, whose key comes from a
 * file) has the scope of eight zero bytes, and takes only keys stored with it.
 *
 * Session reset. A gateway that restarts restores the sessions it kept, but
 * places each one's upward counter past a margin, so that nothing recorded
 * before the restart can be replayed after it; a board still counting below
 * that mark would go unheard. The gateway asks it, sealed under that session,
 *
 *     SESSION_RESET  [reason(1)]       1 = the gateway restarted
 *
 * and a board that opens it under its current session — K_down, a counter
 * greater than the last accepted — drops the session and says hello again at
 * once. Unsealed, or under any other session, it is refused like any frame.
 *
 * Whoever holds a board's old key and recorded the REKEY frame can unwrap the new
 * key: rotation ends the use of a key that leaks later, not of one that was
 * already stolen while the attacker listened (there is no forward secrecy).
 *
 * Nothing here allocates; every buffer is the caller's or the instance's.
 *
 * @project RoboHub Platform
 * @version 0.1
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INCLUDE_ROBOHUB_SECURE_H
#define INCLUDE_ROBOHUB_SECURE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "error.h"

/** The message ids of the handshake. */
#define RH_SECURE_MSG_HELLO   0x0011u
#define RH_SECURE_MSG_SESSION 0x0012u
/** Key rotation; both are only ever sent sealed (robohub/protocol MessageId). */
#define RH_SECURE_MSG_REKEY     0x0014u
#define RH_SECURE_MSG_REKEY_ACK 0x0015u
/** The gateway asks for a new session; only ever sent sealed, gateway to board. */
#define RH_SECURE_MSG_SESSION_RESET 0x0016u

/** A secured frame's message id has this bit set. */
#define RH_SECURE_FLAG 0x8000u

#define RH_SECURE_UID_SIZE   8u
#define RH_SECURE_KEY_SIZE   16u
#define RH_SECURE_NONCE_SIZE 8u
#define RH_SECURE_SCOPE_SIZE 8u
#define RH_SECURE_TAG_SIZE   8u
#define RH_SECURE_HELLO_SIZE   (RH_SECURE_UID_SIZE + RH_SECURE_NONCE_SIZE + RH_SECURE_TAG_SIZE)
#define RH_SECURE_SESSION_SIZE (RH_SECURE_UID_SIZE + 2u + RH_SECURE_NONCE_SIZE + 2u + RH_SECURE_TAG_SIZE)
/** What a secured frame adds to a payload: sid, counter and tag. */
#define RH_SECURE_OVERHEAD (2u + 4u + RH_SECURE_TAG_SIZE)
#define RH_SECURE_REKEY_SIZE     (4u + RH_SECURE_KEY_SIZE)
#define RH_SECURE_REKEY_ACK_SIZE (4u + 8u)

/**
 * @brief A source of nonces: unique per call for the life of the device.
 *
 * A platform with a hardware random source fills `out` from it; one without
 * (an 8-bit part) mixes a boot counter it keeps in non-volatile memory with the
 * time since boot, which never repeats a pair. Returns 0 when it filled `out`.
 */
typedef int (*RoboHubSecureNonce)(rh_u8* out, rh_size length);

/** Milliseconds since boot, for a session's lifetime. */
typedef rh_u32 (*RoboHubSecureClock)(void);

/**
 * @brief Where a rotated key is kept, when an instance keeps it somewhere of its
 * own rather than in the platform's keystore (a Linux host node rewrites its key
 * file). Returns 0 when the key and its epoch are stored durably.
 */
typedef int (*RoboHubSecureStore)(const rh_u8 key[RH_SECURE_KEY_SIZE], rh_u32 epoch, void* user);

typedef enum {
    RH_SECURE_NONE = 0,
    RH_SECURE_HELLO_SENT = 1,
    RH_SECURE_ESTABLISHED = 2,
} RoboHubSecureState;

/** One board's side of its link to a gateway. A value the caller owns. */
typedef struct RoboHubSecure {
    rh_u8  uid[RH_SECURE_UID_SIZE];
    rh_u8  device_key[RH_SECURE_KEY_SIZE];
    rh_u8  nonce_b[RH_SECURE_NONCE_SIZE];
    rh_u16 session_id;
    rh_u8  key_tx[RH_SECURE_KEY_SIZE];
    rh_u8  key_rx[RH_SECURE_KEY_SIZE];
    rh_u32 tx_counter;
    rh_u32 rx_counter;
    rh_u32 expires_ms;
    RoboHubSecureState state;
    RoboHubSecureNonce nonce;
    RoboHubSecureClock now_ms;
    /** Frames refused for a stale session, a replayed counter or a bad tag. */
    rh_u32 rejected;
    /** The epoch of device_key. */
    rh_u32 epoch;
    /** The build's key scope, stored with a rotated key; zeros when it has none. */
    rh_u8  scope[RH_SECURE_SCOPE_SIZE];
    /** A key a REKEY gave and the store kept, used from the next hello on. */
    rh_u8  next_key[RH_SECURE_KEY_SIZE];
    rh_u32 next_epoch;
    rh_u8  has_next;
    /** The instance's own store for a rotated key, or 0 for rh_keystore_store. */
    RoboHubSecureStore store;
    void*  store_user;
} RoboHubSecure;

/** Bind a board's id and device key, and how it gets nonces and the time. */
RoboHubError robohub_secure_init(RoboHubSecure* secure,
                                 const rh_u8 uid[RH_SECURE_UID_SIZE],
                                 const rh_u8 device_key[RH_SECURE_KEY_SIZE],
                                 RoboHubSecureNonce nonce,
                                 RoboHubSecureClock now_ms);

/**
 * @brief Bind a board's id and the device key it was built with, of epoch
 * `epoch` (RH_DEVICE_KEY_EPOCH in robohub_secret.h), and the build's key scope
 * (RH_KEY_SCOPE, or 0 for a build without one). The platform's keystore is read,
 * and a key it holds of a newer epoch under the same scope is used instead.
 */
RoboHubError robohub_secure_init_epoch(RoboHubSecure* secure,
                                       const rh_u8 uid[RH_SECURE_UID_SIZE],
                                       const rh_u8 device_key[RH_SECURE_KEY_SIZE],
                                       rh_u32 epoch,
                                       const rh_u8* scope,
                                       RoboHubSecureNonce nonce,
                                       RoboHubSecureClock now_ms);

/**
 * @brief Keep a rotated key with `store` instead of the platform's keystore.
 * Set it after init; 0 goes back to the keystore.
 */
RoboHubError robohub_secure_on_store(RoboHubSecure* secure, RoboHubSecureStore store, void* user);

/**
 * @brief Act on a REKEY payload that robohub_secure_open has just opened (its
 * session id and counter are the ones the key was wrapped with), and write the
 * REKEY_ACK payload to send back sealed under the same session. The ack says
 * "cannot rotate" (epoch 0, a zero proof) when the key could not be stored; the
 * call still succeeds then, since the answer must be sent. A REKEY for an epoch
 * not newer than the key in use is refused.
 */
RoboHubError robohub_secure_rekey(RoboHubSecure* secure, const rh_u8* payload, rh_size length,
                                  rh_u8 ack[RH_SECURE_REKEY_ACK_SIZE]);

/**
 * @brief Drop the session, after robohub_secure_open has just opened a
 * SESSION_RESET under it: the state goes back to needing a hello and the
 * session's keys are wiped. A key a REKEY gave is kept for that hello.
 */
RoboHubError robohub_secure_reset(RoboHubSecure* secure);

/** Whether frames can be sealed: a session is established and has not expired. */
int robohub_secure_ready(const RoboHubSecure* secure);

/** Write a HELLO payload (RH_SECURE_HELLO_SIZE bytes) with a fresh nonce. */
RoboHubError robohub_secure_hello(RoboHubSecure* secure, rh_u8 out[RH_SECURE_HELLO_SIZE]);

/** Accept a SESSION payload answering the last HELLO; anything else is refused. */
RoboHubError robohub_secure_accept(RoboHubSecure* secure, const rh_u8* payload, rh_size length);

/**
 * @brief Seal a payload for sending: writes sid, counter, payload and tag to
 * `out` and returns their length in `out_length`. The message id to send is
 * `msg_id | RH_SECURE_FLAG`.
 */
RoboHubError robohub_secure_seal(RoboHubSecure* secure, rh_u16 msg_id, rh_u16 tx_id,
                                 const rh_u8* payload, rh_size length,
                                 rh_u8* out, rh_size capacity, rh_size* out_length);

/**
 * @brief Open a received secured payload. On success `inner` and `inner_length`
 * name the payload within `payload`; on any failure the frame must be dropped.
 * `msg_id` is the id as received, flag included.
 */
RoboHubError robohub_secure_open(RoboHubSecure* secure, rh_u16 msg_id, rh_u16 tx_id,
                                 const rh_u8* payload, rh_size length,
                                 const rh_u8** inner, rh_size* inner_length);

/** HMAC-SHA256 truncated to `out_length` bytes; exposed for tests and tools. */
void robohub_secure_hmac(const rh_u8* key, rh_size key_length,
                         const rh_u8* message, rh_size message_length,
                         rh_u8* out, rh_size out_length);

/**
 * @brief The platform's nonce source. Weak by default and failing, so a build
 * with no source establishes no session rather than one with predictable nonces;
 * every platform overrides it.
 */
int robohub_platform_nonce(rh_u8* out, rh_size length);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ROBOHUB_SECURE_H */
