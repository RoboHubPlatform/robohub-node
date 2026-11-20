/**
 * @file test_vectors.c
 * @brief The board's sealed frames against the shared known-answer vectors.
 *
 * tests/vectors/secure.json is written once by tests/vectors/generate.py from
 * the protocol in secure.h, and robohub-gateway checks its own side against the
 * same file. This checks the board's side byte for byte: the hello it says, the
 * session it takes, the keys it derives from it, a reading it seals, the
 * gateway's frames it opens (a command, a SESSION_RESET, a REKEY) and the
 * REKEY_ACK with its proof — through robohub_secure_* and, for whole frames on
 * the wire, through the transport. Nothing here is computed from the protocol a
 * second time: what the board writes is compared with the file.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "robohub/secure.h"
#include "robohub/transport.h"
#include "keystore_double.h"

#ifndef ROBOHUB_VECTORS
#define ROBOHUB_VECTORS "tests/vectors/secure.json"
#endif

static int failures = 0;
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", what, __LINE__); failures++; } \
                               else { printf("[PASS] %s\n", what); } } while (0)

/* ---- reading the vectors -------------------------------------------------- */

static char* json_text;

/* The text of one field of one top-level section: a section is an object of
 * scalars, so its end is the first '}' after its start. Returns 0 when absent. */
static const char* field_at(const char* section, const char* field, const char** end)
{
    char key[64];
    snprintf(key, sizeof(key), "\"%s\": {", section);
    const char* start = strstr(json_text, key);
    if (start == 0) return 0;
    const char* close = strchr(start, '}');
    snprintf(key, sizeof(key), "\"%s\": ", field);
    const char* at = strstr(start, key);
    if (at == 0 || at > close) return 0;
    at += strlen(key);
    if (end) *end = close;
    return at;
}

/* A hex field as bytes; returns the length, or 0 when the field is absent. */
static size_t vec_bytes(const char* section, const char* field, rh_u8* out, size_t capacity)
{
    const char* at = field_at(section, field, 0);
    if (at == 0 || *at != '"') {
        fprintf(stderr, "vectors: no %s.%s\n", section, field);
        failures++;
        return 0;
    }
    at++;
    size_t n = 0;
    while (at[0] != '"' && n < capacity) {
        unsigned v;
        if (sscanf(at, "%2x", &v) != 1) break;
        out[n++] = (rh_u8)v;
        at += 2;
    }
    return n;
}

static unsigned long vec_number(const char* section, const char* field)
{
    const char* at = field_at(section, field, 0);
    if (at == 0) {
        fprintf(stderr, "vectors: no %s.%s\n", section, field);
        failures++;
        return 0;
    }
    return strtoul(at, 0, 10);
}

static int load(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == 0) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    json_text = (char*)malloc((size_t)size + 1u);
    if (json_text == 0 || fread(json_text, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    json_text[size] = 0;
    fclose(f);
    return 0;
}

/* ---- the board's surroundings --------------------------------------------- */

static rh_u8 next_nonce[8];
static int vector_nonce(rh_u8* out, rh_size n) { memcpy(out, next_nonce, n); return 0; }
static rh_u32 clock_now(void) { return 1000u; }

static rh_u8 captured[2u * RH_TRANSPORT_MAX_FRAME_SIZE + 4u];
static rh_size captured_len;
static int capture_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    if (length <= sizeof(captured)) {
        memcpy(captured, data, length);
        captured_len = length;
    }
    return 0;
}

/* Whether the payload of a sealed vector is what the board sealed. */
static int sealed_matches(RoboHubSecure* board, const char* section)
{
    rh_u8 inner[64], want[96], out[96];
    size_t inner_len = vec_bytes(section, "inner", inner, sizeof(inner));
    size_t want_len = vec_bytes(section, "payload", want, sizeof(want));
    rh_size out_len = 0;
    board->tx_counter = (rh_u32)vec_number(section, "counter") - 1u;
    if (robohub_secure_seal(board, (rh_u16)vec_number(section, "msg_id"), (rh_u16)vec_number(section, "tx_id"),
                            inner, inner_len, out, sizeof(out), &out_len) != ROBOHUB_OK) {
        return 0;
    }
    return out_len == want_len && memcmp(out, want, want_len) == 0;
}

/* Whether the board opens a sealed vector, and finds its inner payload in it. */
static int opens_to_inner(RoboHubSecure* board, const char* section, const rh_u8** inner_out, rh_size* inner_len_out)
{
    rh_u8 payload[96], want[64];
    size_t n = vec_bytes(section, "payload", payload, sizeof(payload));
    size_t want_len = vec_bytes(section, "inner", want, sizeof(want));
    static rh_u8 kept[96];
    memcpy(kept, payload, n);
    const rh_u8* inner = 0;
    rh_size inner_len = 0;
    if (robohub_secure_open(board, (rh_u16)vec_number(section, "wire_msg_id"), (rh_u16)vec_number(section, "tx_id"),
                            kept, n, &inner, &inner_len) != ROBOHUB_OK) {
        return 0;
    }
    if (inner_out) *inner_out = inner;
    if (inner_len_out) *inner_len_out = inner_len;
    return inner_len == want_len && memcmp(inner, want, want_len) == 0;
}

static int captured_is(const char* section)
{
    rh_u8 want[160];
    size_t n = vec_bytes(section, "frame", want, sizeof(want));
    return captured_len == n && memcmp(captured, want, n) == 0;
}

static void feed_vector_frame(RoboHubTransport* transport, const char* section)
{
    rh_u8 bytes[160];
    size_t n = vec_bytes(section, "frame", bytes, sizeof(bytes));
    robohub_transport_feed(transport, bytes, n);
}

int main(int argc, char** argv)
{
    const char* path = argc > 1 ? argv[1] : ROBOHUB_VECTORS;
    if (load(path) != 0) {
        fprintf(stderr, "FAIL: cannot read the vectors at %s\n", path);
        return 1;
    }

    rh_u8 k_gw[32], uid[8], k0[16], k1[16], derived[16];
    vec_bytes("keys", "k_gw", k_gw, sizeof(k_gw));
    vec_bytes("keys", "uid", uid, sizeof(uid));
    vec_bytes("keys", "k_dev_epoch0", k0, sizeof(k0));
    vec_bytes("keys", "k_dev_epoch1", k1, sizeof(k1));

    /* The device keys, from the library's HMAC (the gateway derives them; the
     * board is built with them, so this holds the HMAC and the labels). */
    {
        rh_u8 m[32];
        memcpy(m, "rh-dev1", 7); memcpy(m + 7, uid, 8);
        robohub_secure_hmac(k_gw, 32, m, 15, derived, 16);
        CHECK(memcmp(derived, k0, 16) == 0, "vectors: K_dev of epoch 0");
        rh_u8 e1[4] = { 1, 0, 0, 0 };
        memcpy(m, "rh-dev2", 7); memcpy(m + 7, uid, 8); memcpy(m + 15, e1, 4);
        robohub_secure_hmac(k_gw, 32, m, 19, derived, 16);
        CHECK(memcmp(derived, k1, 16) == 0, "vectors: K_dev of epoch 1");
    }

    /* The handshake, in the secure layer. */
    keystore_double_reset();
    RoboHubSecure board;
    rh_u8 want[96], hello[RH_SECURE_HELLO_SIZE], session[RH_SECURE_SESSION_SIZE];
    vec_bytes("hello", "nonce_b", next_nonce, sizeof(next_nonce));
    CHECK(robohub_secure_init(&board, uid, k0, vector_nonce, clock_now) == ROBOHUB_OK, "vectors: init");
    CHECK(robohub_secure_hello(&board, hello) == ROBOHUB_OK, "vectors: hello");
    vec_bytes("hello", "payload", want, sizeof(want));
    CHECK(memcmp(hello, want, sizeof(hello)) == 0, "vectors: the hello's bytes");
    vec_bytes("session", "payload", session, sizeof(session));
    CHECK(robohub_secure_accept(&board, session, sizeof(session)) == ROBOHUB_OK, "vectors: the session is accepted");
    CHECK(board.session_id == (rh_u16)vec_number("session", "sid"), "vectors: the session id");
    vec_bytes("session", "k_up", want, sizeof(want));
    CHECK(memcmp(board.key_tx, want, 16) == 0, "vectors: K_up");
    vec_bytes("session", "k_down", want, sizeof(want));
    CHECK(memcmp(board.key_rx, want, 16) == 0, "vectors: K_down");

    /* Board to gateway: a reading, sealed at its counter. */
    CHECK(sealed_matches(&board, "reading"), "vectors: a sealed reading's bytes");

    /* Gateway to board: SESSION_RESET opens (on a copy: it ends the session). */
    {
        RoboHubSecure copy = board;
        CHECK(opens_to_inner(&copy, "session_reset", 0, 0), "vectors: SESSION_RESET opens to its reason");
        CHECK(robohub_secure_reset(&copy) == ROBOHUB_OK && !robohub_secure_ready(&copy), "vectors: and ends the session");
    }

    /* REKEY: unwrapped to the epoch-1 key, answered with the proof. */
    {
        const rh_u8* inner = 0; rh_size inner_len = 0;
        rh_u8 ack[RH_SECURE_REKEY_ACK_SIZE];
        CHECK(opens_to_inner(&board, "rekey", &inner, &inner_len), "vectors: REKEY opens");
        CHECK(robohub_secure_rekey(&board, inner, inner_len, ack) == ROBOHUB_OK, "vectors: REKEY is taken");
        CHECK(keystore_double_stores == 1 && keystore_double_key.epoch == 1u &&
              memcmp(keystore_double_key.key, k1, 16) == 0, "vectors: REKEY unwraps the epoch-1 key");
        vec_bytes("rekey_ack", "inner", want, sizeof(want));
        CHECK(memcmp(ack, want, sizeof(ack)) == 0, "vectors: REKEY_ACK's epoch and proof");
        CHECK(sealed_matches(&board, "rekey_ack"), "vectors: a sealed REKEY_ACK's bytes");
    }

    /* A command down, and its acknowledgement up. */
    CHECK(opens_to_inner(&board, "control", 0, 0), "vectors: a command opens to its bytes");
    CHECK(sealed_matches(&board, "control_ack"), "vectors: a sealed acknowledgement's bytes");

    /* A tampered vector does not open: the comparison above is not vacuous. */
    {
        rh_u8 payload[96];
        size_t n = vec_bytes("control", "payload", payload, sizeof(payload));
        payload[n - 1] ^= 1;
        const rh_u8* inner = 0; rh_size inner_len = 0;
        RoboHubSecure fresh = board;
        fresh.rx_counter = 0;
        CHECK(robohub_secure_open(&fresh, (rh_u16)vec_number("control", "wire_msg_id"), 1, payload, n,
                                  &inner, &inner_len) != ROBOHUB_OK, "vectors: a changed tag does not open");
    }

    /* The hello of a board holding the epoch-1 key. */
    {
        RoboHubSecure rotated;
        keystore_double_reset();
        vec_bytes("hello_epoch1", "nonce_b", next_nonce, sizeof(next_nonce));
        CHECK(robohub_secure_init_epoch(&rotated, uid, k1, 1u, 0, vector_nonce, clock_now) == ROBOHUB_OK &&
              robohub_secure_hello(&rotated, hello) == ROBOHUB_OK, "vectors: an epoch-1 hello");
        vec_bytes("hello_epoch1", "payload", want, sizeof(want));
        CHECK(memcmp(hello, want, sizeof(hello)) == 0, "vectors: the epoch-1 hello's bytes");
    }

    /* Whole frames on the wire, through the transport. */
    {
        keystore_double_reset();
        RoboHubSecure secure;
        RoboHubTransport transport;
        const RoboHubTransportPhy phy = { .write = capture_write, .context = 0 };
        vec_bytes("hello", "nonce_b", next_nonce, sizeof(next_nonce));
        robohub_secure_init(&secure, uid, k0, vector_nonce, clock_now);
        CHECK(robohub_transport_init(&transport, &phy) == ROBOHUB_OK &&
              robohub_transport_secure(&transport, &secure) == ROBOHUB_OK, "vectors: a transport");
        rh_u8 reading[64];
        size_t reading_len = vec_bytes("reading", "inner", reading, sizeof(reading));
        captured_len = 0;
        CHECK(robohub_transport_send(&transport, 0x0201u, 0u, reading, (rh_u16)reading_len) == ROBOHUB_ERR_STATE &&
              captured_is("hello"), "vectors: the hello frame on the wire");
        feed_vector_frame(&transport, "session");
        CHECK(robohub_secure_ready(&secure), "vectors: the session frame is taken off the wire");
        secure.tx_counter = (rh_u32)vec_number("reading", "counter") - 1u;
        CHECK(robohub_transport_send(&transport, 0x0201u, 0u, reading, (rh_u16)reading_len) == ROBOHUB_OK &&
              captured_is("reading"), "vectors: the sealed reading frame on the wire");
        secure.tx_counter = (rh_u32)vec_number("rekey_ack", "counter") - 1u;
        captured_len = 0;
        feed_vector_frame(&transport, "rekey");
        CHECK(captured_is("rekey_ack"), "vectors: a REKEY frame answered with the REKEY_ACK frame");
        rh_u8 ack[32];
        size_t ack_len = vec_bytes("control_ack", "inner", ack, sizeof(ack));
        secure.tx_counter = (rh_u32)vec_number("control_ack", "counter") - 1u;
        CHECK(robohub_transport_send(&transport, 0x0302u, 1u, ack, (rh_u16)ack_len) == ROBOHUB_OK &&
              captured_is("control_ack"), "vectors: the acknowledgement frame on the wire");
    }

    free(json_text);
    if (failures == 0) {
        printf("test_vectors: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
