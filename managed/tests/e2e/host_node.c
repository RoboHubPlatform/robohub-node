/*
 * host_node.c — a Linux host board that takes updates over its link, for the
 * end-to-end update test (RNODE-UPD-0007, e2e_update.py).
 *
 * The real platform (robohub-linux, its file slot), core, link and managed layer;
 * the link is uart0 (RHO_UART0, a pseudo-terminal the test holds). What a build
 * would compile in comes from the environment, so one binary serves every run:
 *
 *   RH_TEST_UID        the board's uid, 16 hex digits
 *   RH_TEST_UPDATE_KEY the mesh's update public key, 64 hex digits
 *   RH_TEST_FIRMWARE   the firmware version it reports
 *   RH_TEST_CRASH      when set, an image on trial ends before it is confirmed
 *
 *   RH_TEST_CONFIRM_MS the healthy time before an image is kept, short so the
 *                      test does not wait 30 s
 *   RH_TEST_TRIAL_MS   the longest an image stays on trial unconfirmed
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "platform.h"
#include "registry.h"
#include "robohub/hal/uart.h"
#include "robohub/managed.h"
#include "robohub/transport.h"
#include "robohub/update_slot.h"
#include "robohub/core/core.h"
#include "robohub/core/scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RoboHubUART* uart;
static RoboHubTransport link;

static int uart_phy_write(const rh_u8* data, rh_size length, void* context)
{
    (void)context;
    return uart != 0 && uart->write(uart, data, length) == ROBOHUB_OK ? 0 : -1;
}

static RoboHubError link_task(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    rh_u8 buffer[256];
    rh_size n;
    while (uart->read_available != 0 && (n = uart->read_available(uart, buffer, sizeof buffer)) > 0u) {
        robohub_transport_feed(&link, buffer, n);
    }
    if (getenv("RH_TEST_CRASH") != 0 && rh_update_pending(0)) {
        fprintf(stderr, "host-node: crashing on trial\n");
        exit(1);
    }
    return ROBOHUB_OK;
}

static int unhex(const char* text, rh_u8* out, size_t n)
{
    if (text == 0 || strlen(text) != 2u * n) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(text + 2u * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (rh_u8)v;
    }
    return 0;
}

int main(void)
{
    if (robohub_platform_init() != ROBOHUB_OK) {
        return 2;
    }
    uart = robohub_get_uart("uart0");
    if (uart == 0 || uart->init(uart, 115200u) != ROBOHUB_OK) {
        fprintf(stderr, "host-node: no uart0\n");
        return 2;
    }
    static rh_u8 uid[8], key[32];
    if (unhex(getenv("RH_TEST_UID"), uid, sizeof uid) != 0 || unhex(getenv("RH_TEST_UPDATE_KEY"), key, sizeof key) != 0) {
        fprintf(stderr, "host-node: RH_TEST_UID and RH_TEST_UPDATE_KEY are needed\n");
        return 2;
    }
    const RoboHubTransportPhy phy = { .write = uart_phy_write, .context = 0 };
    (void)robohub_transport_init(&link, &phy);
    const char* firmware = getenv("RH_TEST_FIRMWARE");
    RoboHubManagedConfig config = {
        .transport = &link,
        .uid = uid,
        .update_key = key,
        .firmware_version = (rh_u16)(firmware != 0 ? atoi(firmware) : 1),
        .confirm_ms = getenv("RH_TEST_CONFIRM_MS") != 0 ? (rh_u32)atoi(getenv("RH_TEST_CONFIRM_MS")) : 0u,
        .trial_ms = getenv("RH_TEST_TRIAL_MS") != 0 ? (rh_u32)atoi(getenv("RH_TEST_TRIAL_MS")) : 0u,
    };
    (void)robohub_managed_init(&config);
    if (robohub_core_init(0) != ROBOHUB_OK) {
        fprintf(stderr, "host-node: the core did not start\n");
        return 2;
    }
    robohub_core_task_id task;
    (void)robohub_core_task_add("link", link_task, 0, 5u, &task);
    rh_update_info info;
    fprintf(stderr, "host-node: up, slot %d%s\n", rh_update_running_slot(), rh_update_pending(&info) ? ", on trial" : "");
    (void)robohub_core_run();
    return 0;
}
