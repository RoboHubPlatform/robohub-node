/*
 * test_health_sealed.c — on a sealed link, HEALTH waits for the session: with no
 * gateway answering the hello, nothing but hellos goes out (RNODE-HEALTH-0004).
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "health_fixture.h"

static RoboHubDeviceNode node;
static int passes;

int robohub_platform_nonce(rh_u8* out, rh_size length)
{
    for (rh_size i = 0; i < length; i++) {
        out[i] = (rh_u8)(0x5Au + i);
    }
    return 0;
}

static RoboHubError stopper(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    if (++passes == 600) { /* six seconds: the hello is repeated */
        robohub_core_stop();
    }
    return ROBOHUB_OK;
}

static RoboHubError test_start(void)
{
    return robohub_core_task_add("stopper", stopper, 0, 10u, 0);
}

static const robohub_core_app test_app = { .name = "test", .start = test_start };

// @verifies RNODE-HEALTH-0004
int main(void)
{
    static const rh_u8 uid[RH_SECURE_UID_SIZE] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const rh_u8 key[RH_SECURE_KEY_SIZE] = { 9 };
    assert(robohub_register_timer("system_timer", &fx_timer) == ROBOHUB_OK);

    RoboHubDeviceNodeConfig config;
    memset(&config, 0, sizeof config);
    config.phy.write = fx_write;
    config.read = fx_read;
    config.secure_uid = uid;
    config.secure_key = key;
    assert(robohub_device_node_init(&node, &config) == ROBOHUB_OK);
    assert(robohub_core_app_register(&test_app) == ROBOHUB_OK);

    /* Something to publish, so the link opens its handshake. */
    RoboHubDeviceId id = { .area_id = 7, .node_id = 21 };
    assert(robohub_device_node_add_device(&node, id, 0, 0) == ROBOHUB_OK);

    (void)robohub_device_node_run(&node);
    assert(passes == 600);
    assert(!node.transport.health_sent);

    static fx_frame frames[256];
    int count = fx_frames(frames, 256);
    for (int i = 0; i < count; i++) {
        assert(frames[i].msg_id != MESSAGE_ID_HEALTH);
    }
    return 0;
}
