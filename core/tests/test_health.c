/*
 * test_health.c — the runtime's health on a fake board: a fake clock, a fake
 * watchdog, fake reset flags and retained memory (RNODE-HEALTH-0001..0003,
 * RNODE-HEALTH-0005).
 *
 * 1. A whole run: health is read before any application's init and the record
 *    is taken once; the watchdog starts with the period the platform chose; the
 *    loop feeds it, and does not feed it after a pass in which a task blocked
 *    for longer than the allowance; a fault records the task that was running.
 * 2. Tasks as threads (white-box): one task stops while the other keeps going;
 *    feeding stops once the stopped one is past its allowance, and starts again
 *    when it declares a longer one.
 * 3. A period of 0, and a platform without a watchdog: nothing is started.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "robohub/core/core.h"
#include "robohub/core/app.h"
#include "robohub/core/health.h"
#include "robohub/core/scheduler.h"
#include "core_internal.h"

#include <assert.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  The fake board                                                            */
/* -------------------------------------------------------------------------- */

static rh_u32 clock_ms;
static rh_bool wd_present = RH_TRUE;
static int wd_starts;
static rh_u32 wd_asked_ms;
static rh_u32 feeds;
static rh_u32 last_feed_ms;
static rh_u32 feeds_at[4096];
static rh_u32 retained[RH_HEALTH_RETAINED_SIZE / 4u];

void rh_watchdog_caps_get(rh_watchdog_caps* caps)
{
    memset(caps, 0, sizeof *caps);
    caps->present = wd_present;
    caps->min_ms = 1;
    caps->max_ms = 5000; /* asked for 8000, it gives its longest */
}

RoboHubError rh_watchdog_start(uint32_t period_ms, uint32_t* chosen_ms)
{
    wd_starts++;
    wd_asked_ms = period_ms;
    *chosen_ms = period_ms > 5000u ? 5000u : period_ms;
    return ROBOHUB_OK;
}

void rh_watchdog_feed(void)
{
    if (feeds < sizeof feeds_at / sizeof feeds_at[0]) {
        feeds_at[feeds] = clock_ms;
    }
    feeds++;
    last_feed_ms = clock_ms;
}

rh_reset_reason rh_platform_reset_reason(void) { return RH_RESET_WATCHDOG; }
void* rh_platform_retained(void) { return retained; }

static rh_millis fake_millis(RoboHubTimer* t) { (void)t; return clock_ms; }
static void fake_delay(RoboHubTimer* t, rh_u32 ms) { (void)t; clock_ms += ms; }
static RoboHubTimer fake_timer = { .name = "fake", .get_millis = fake_millis, .delay_ms = fake_delay };

/* -------------------------------------------------------------------------- */
/*  1. A whole run                                                            */
/* -------------------------------------------------------------------------- */

static rh_bool init_saw_health;
static robohub_core_task_id task_a, task_b;
static rh_u32 stall_end;

static RoboHubError app_init(void)
{
    robohub_core_health h;
    robohub_core_health_get(&h);
    /* Read before this init: the record the last boot left is already here. */
    init_saw_health = h.has_fault && h.fault.code == RH_FAULT_HARD && h.fault.pc == 0x1234u;
    return ROBOHUB_OK;
}

static const robohub_core_app app = { .name = "health", .init = app_init };

static RoboHubError run_a(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    if (clock_ms >= 500u && clock_ms < 600u) {
        /* An assertion fails: the record names this task and this pass. */
        rh_health_fault(RH_FAULT_ASSERT, 0x42u);
    }
    if (clock_ms >= 30000u) {
        robohub_core_stop();
    }
    return ROBOHUB_OK;
}

static RoboHubError run_b(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    if (stall_end == 0u && clock_ms >= 1000u) {
        clock_ms += 20000u; /* a bus that does not answer */
        stall_end = clock_ms;
    }
    return ROBOHUB_OK;
}

static void whole_run(void)
{
    /* The boot before this one faulted. */
    rh_health_fault(RH_FAULT_HARD, 0x1234u);

    assert(robohub_core_app_register(&app) == ROBOHUB_OK);
    robohub_core_config config = { .timer = &fake_timer };
    assert(robohub_core_init(&config) == ROBOHUB_OK);
    assert(init_saw_health);

    robohub_core_health h;
    robohub_core_health_get(&h);
    assert(h.reset_reason == RH_RESET_WATCHDOG);
    assert(h.boot_count == 1u);
    /* Taken once: the contract has nothing left to give. */
    rh_fault_record again;
    assert(!rh_health_fault_take(&again));

    assert(robohub_core_task_add("a", run_a, 0, 100u, &task_a) == ROBOHUB_OK);
    assert(robohub_core_task_add("b", run_b, 0, 100u, &task_b) == ROBOHUB_OK);
    assert(robohub_core_run() == ROBOHUB_OK);

    robohub_core_health_get(&h);
    assert(wd_starts == 1 && wd_asked_ms == 8000u);
    assert(h.watchdog_ms == 5000u);
    assert(h.watchdog_feeds == feeds && feeds > 10u);
    /* Fed before the stall, not on the pass that stalled, fed again after. */
    rh_bool before = RH_FALSE, at_stall = RH_FALSE, after = RH_FALSE;
    for (rh_u32 i = 0; i < feeds && i < 4096u; i++) {
        if (feeds_at[i] < 1000u) before = RH_TRUE;
        if (feeds_at[i] == stall_end) at_stall = RH_TRUE;
        if (feeds_at[i] > stall_end) after = RH_TRUE;
    }
    assert(before && !at_stall && after);
    assert(h.late_task == task_a || h.late_task == task_b);

    /* The assertion in task a left a record naming it, for the next boot. */
    rh_fault_record r;
    assert(rh_health_fault_take(&r));
    assert(r.code == RH_FAULT_ASSERT && r.pc == 0x42u && r.task == task_a);
    assert(r.uptime_ms >= 500u && r.uptime_ms < 600u);
}

/* -------------------------------------------------------------------------- */
/*  2. Tasks as threads                                                       */
/* -------------------------------------------------------------------------- */

static RoboHubError noop(robohub_core_task_id id, void* user)
{
    (void)id; (void)user;
    return ROBOHUB_OK;
}

static void threads(void)
{
    robohub_core_scheduler_reset();
    robohub_core_task_id t1, t2;
    assert(robohub_core_task_add("t1", noop, 0, 100u, &t1) == ROBOHUB_OK);
    assert(robohub_core_task_add("t2", noop, 0, 100u, &t2) == ROBOHUB_OK);

    robohub_core_watchdog_configure(1000u);
    wd_starts = 0;
    robohub_core_watchdog_begin();
    assert(wd_starts == 1);

    /* t2's thread stops at 1000. Its allowance is 2 x 100 + 1000 = 1200. */
    rh_u32 last_fed = 0u;
    robohub_core_health h;
    for (clock_ms = 0u; clock_ms <= 4000u; clock_ms += 50u) {
        robohub_core_scheduler_note_run(t1, clock_ms);
        if (clock_ms <= 1000u) {
            robohub_core_scheduler_note_run(t2, clock_ms);
        }
        rh_u32 before = feeds;
        robohub_core_watchdog_service(clock_ms);
        if (feeds != before) {
            last_fed = clock_ms;
        }
    }
    assert(last_fed == 2200u);
    robohub_core_health_get(&h);
    assert(h.late_task == t2);

    /* A task that is meant to be slow says so, and the loop feeds again. */
    assert(robohub_core_task_set_allowance(t2, 10000u) == ROBOHUB_OK);
    rh_u32 before = feeds;
    robohub_core_watchdog_service(clock_ms);
    assert(feeds == before + 1u);
    assert(robohub_core_task_set_allowance(0xBEEFu, 1u) == ROBOHUB_ERR_NOT_FOUND);
}

/* -------------------------------------------------------------------------- */
/*  3. Nothing to start                                                       */
/* -------------------------------------------------------------------------- */

static void nothing_started(void)
{
    robohub_core_health h;

    robohub_core_watchdog_configure(0u);
    wd_starts = 0;
    robohub_core_watchdog_begin();
    robohub_core_health_get(&h);
    assert(wd_starts == 0 && h.watchdog_ms == 0u);
    rh_u32 before = feeds;
    robohub_core_watchdog_service(clock_ms);
    assert(feeds == before);

    robohub_core_watchdog_configure(8000u);
    wd_present = RH_FALSE;
    robohub_core_watchdog_begin();
    robohub_core_health_get(&h);
    assert(wd_starts == 0 && h.watchdog_ms == 0u);
}

// @verifies RNODE-HEALTH-0001, RNODE-HEALTH-0002, RNODE-HEALTH-0003, RNODE-HEALTH-0005
int main(void)
{
    whole_run();
    threads();
    nothing_started();
    return 0;
}
