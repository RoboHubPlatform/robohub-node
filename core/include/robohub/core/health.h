/**
 * @file health.h
 * @brief What the runtime knows about the board's health: why it restarted,
 *        how many times, what failed last, and how the watchdog is kept.
 *
 * Read once at boot, before any application's `init` (RNODE-HEALTH-0002), from
 * robohub-common's health contract (robohub/health.h). The fault record is
 * taken then, so it is the runtime's copy here and nowhere else: an application
 * reads it from this API, and the link reports it in HEALTH when the link is
 * chosen (RNODE-HEALTH-0004).
 *
 * The watchdog (RNODE-HEALTH-0001) is started when the loop begins, with
 * RH_CORE_WATCHDOG_MS (`CONFIG_RH_CORE_WATCHDOG_MS`, 8000 by default, 0 off), on
 * a platform that has one. The loop feeds it only while every task has run
 * within its allowance (robohub_core_task_set_allowance()).
 *
 * @project RoboHub Platform
 * @author Aurel Miron
 * @date 2026
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef INCLUDE_ROBOHUB_CORE_HEALTH_H
#define INCLUDE_ROBOHUB_CORE_HEALTH_H

#include "types.h"
#include "robohub/health.h"
#include "robohub/core/scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Why the board last restarted. */
    rh_reset_reason reset_reason;
    /** Boots since power was applied, this one included. */
    rh_u32 boot_count;
    /** The previous boot left a fault record. */
    rh_bool has_fault;
    /** That record; zero, with task RH_HEALTH_NO_TASK, when there is none. */
    rh_fault_record fault;
    /** The period the watchdog runs with, as the platform chose it; 0 when it is off. */
    rh_u32 watchdog_ms;
    /** Times the loop fed it. */
    rh_u32 watchdog_feeds;
    /** The task that last kept the loop from feeding it, or RH_CORE_TASK_INVALID. */
    robohub_core_task_id late_task;
    /** Every task is within its allowance now — kept whether or not a watchdog runs. */
    rh_bool on_time;
    /** Since when, in uptime milliseconds, every task has been within it. */
    rh_u32 on_time_since_ms;
} robohub_core_health;

/** The health as the runtime holds it. Zero before robohub_core_init. */
void robohub_core_health_get(robohub_core_health* out);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ROBOHUB_CORE_HEALTH_H */
