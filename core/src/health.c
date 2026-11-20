/**
 * @file health.c
 * @brief The runtime's health: read at boot, and the watchdog kept by the loop.
 *
 * @project RoboHub Platform
 * @author Aurel Miron
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include "robohub/core/health.h"
#include "robohub/core/config.h"
#include "core_internal.h"

static robohub_core_health state;
static rh_u32 configured_ms = (rh_u32)RH_CORE_WATCHDOG_MS;

void robohub_core_watchdog_configure(rh_u32 period_ms)
{
    configured_ms = period_ms;
}

void robohub_core_health_reset(void)
{
    state.reset_reason = RH_RESET_UNKNOWN;
    state.boot_count = 0u;
    state.has_fault = RH_FALSE;
    state.fault.code = 0u;
    state.fault.task = RH_HEALTH_NO_TASK;
    state.fault.pc = 0u;
    state.fault.uptime_ms = 0u;
    state.watchdog_ms = 0u;
    state.watchdog_feeds = 0u;
    state.late_task = RH_CORE_TASK_INVALID;
}

/* Before any application's init: take the record before anything can write
   over it, so it is reported once. */
// @req RNODE-HEALTH-0002
void robohub_core_health_boot(void)
{
    robohub_core_health_reset();
    rh_health_boot();
    state.reset_reason = rh_health_reset_reason();
    state.boot_count = rh_health_boot_count();
    state.has_fault = rh_health_fault_take(&state.fault) ? RH_TRUE : RH_FALSE;
}

// @req RNODE-HEALTH-0001
void robohub_core_watchdog_begin(void)
{
    state.watchdog_ms = 0u;
    if (configured_ms == 0u) {
        return;
    }
    rh_watchdog_caps caps;
    rh_watchdog_caps_get(&caps);
    if (!caps.present) {
        return;
    }
    rh_u32 chosen = 0u;
    if (rh_watchdog_start(configured_ms, &chosen) == ROBOHUB_OK) {
        state.watchdog_ms = chosen != 0u ? chosen : configured_ms;
    }
}

/* Every pass: fed only while every task kept its allowance. A late task is
   kept as the reason, and the board resets when the period runs out. */
// @req RNODE-HEALTH-0001
void robohub_core_watchdog_service(rh_u32 now_ms)
{
    if (state.watchdog_ms == 0u) {
        return;
    }
    robohub_core_task_id late = robohub_core_scheduler_late_task(now_ms, state.watchdog_ms);
    if (late != RH_CORE_TASK_INVALID) {
        state.late_task = late;
        return;
    }
    rh_watchdog_feed();
    state.watchdog_feeds++;
}

void robohub_core_health_get(robohub_core_health* out)
{
    if (out != 0) {
        *out = state;
    }
}
