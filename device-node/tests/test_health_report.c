/*
 * test_health_report.c — a node on a plain link reports its health once per
 * boot, and again when a DIAG_REQUEST asks (RNODE-HEALTH-0004, PROTO-0007).
 *
 * The boot before this one faulted (a stack overflow) and reset through the
 * software path, so the report says FAULT, boot 1, and the record.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "health_fixture.h"

static RoboHubDeviceNode node;
static int passes;
static int diag_sent_at = -1;

static RoboHubError stopper(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    passes++;
    if (passes == 20) {
        /* The gateway asks for diagnostics: the report comes again. */
        DiagRequest request;
        memset(&request, 0, sizeof request);
        request.device.area_id = 7;
        request.device.node_id = 20;
        rh_u8 bytes[DIAG_REQUEST_WIRE_SIZE];
        assert(DiagRequest_serialize(&request, bytes, sizeof bytes) != 0u);
        fx_send_to_node(MESSAGE_ID_DIAG_REQUEST, bytes, (rh_u16)sizeof bytes);
        diag_sent_at = passes;
    }
    if (passes == 60) {
        robohub_core_stop();
    }
    return ROBOHUB_OK;
}

static RoboHubError test_start(void)
{
    return robohub_core_task_add("stopper", stopper, 0, 10u, 0);
}

static const robohub_core_app test_app = { .name = "test", .start = test_start };

static RoboHubError plug(RoboHubDeviceId id, rh_u16 opcode, rh_u32 argument, rh_u32* actual, void* user)
{
    (void)id; (void)opcode; (void)argument; (void)actual; (void)user;
    return ROBOHUB_OK;
}

// @verifies RNODE-HEALTH-0004
int main(void)
{
    rh_health_fault(RH_FAULT_STACK, 0xABCu);
    assert(robohub_register_timer("system_timer", &fx_timer) == ROBOHUB_OK);

    RoboHubDeviceNodeConfig config;
    memset(&config, 0, sizeof config);
    config.phy.write = fx_write;
    config.read = fx_read;
    config.firmware_version = 9u;
    assert(robohub_device_node_init(&node, &config) == ROBOHUB_OK);
    RoboHubDeviceId id = { .area_id = 7, .node_id = 20 };
    assert(robohub_device_node_add_device(&node, id, plug, 0) == ROBOHUB_OK);
    assert(robohub_core_app_register(&test_app) == ROBOHUB_OK);

    (void)robohub_device_node_run(&node);
    assert(passes == 60 && diag_sent_at == 20);
    assert(node.health_sent);

    static fx_frame frames[256];
    int count = fx_frames(frames, 256);
    int health = 0;
    for (int i = 0; i < count; i++) {
        if (frames[i].msg_id != MESSAGE_ID_HEALTH) {
            continue;
        }
        health++;
        const rh_u8* p = frames[i].payload;
        assert(frames[i].length == HEALTH_WIRE_SIZE);
        assert(p[0] == 1u && p[1] == 1u);
        assert(fx_u16(p + 2) == 7u && fx_u16(p + 4) == 20u);
        assert(fx_u32(p + 6) == RESET_REASON_FAULT);
        assert(fx_u32(p + 10) == 1u);              /* boot count */
        assert(fx_u32(p + 14) == RH_FAULT_STACK);  /* fault code */
        assert(fx_u16(p + 18) == RH_HEALTH_NO_TASK);
        assert(fx_u32(p + 20) == 0xABCu);          /* pc */
        assert(fx_u16(p + 28) == 9u);              /* firmware */
    }
    /* Once at boot, once for the diagnostic request. */
    assert(health == 2);
    return 0;
}
