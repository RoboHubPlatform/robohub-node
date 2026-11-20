/*
 * core_internal.h — what the runtime's own sources share.
 *
 * None of this is a contract: it is the seam between the parts of one library,
 * and it is not installed. The public headers are the contract.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#ifndef ROBOHUB_CORE_INTERNAL_H
#define ROBOHUB_CORE_INTERNAL_H

#include "types.h"
#include "error.h"
#include "robohub/core/diagnostics.h"
#include "robohub/core/scheduler.h"

/** No task is due within the foreseeable future. */
#define RH_CORE_NO_DUE 0xFFFFFFFFu

/* The counters, writable by the parts that count. */
robohub_core_diagnostics* robohub_core_diag_state(void);

/* Event bus. */
RoboHubError robohub_core_event_init(void);
void robohub_core_event_reset(void);

/* Scheduler. */
RoboHubError robohub_core_scheduler_init(void);
void robohub_core_scheduler_reset(void);
void robohub_core_scheduler_run(rh_u32 now_ms);
rh_u32 robohub_core_scheduler_next_due(rh_u32 now_ms);
/* When each task last ran, for the watchdog: a task's thread notes it, the
 * cooperative pass notes it itself. The late task is the first one past its
 * allowance at `now_ms`, or RH_CORE_TASK_INVALID when every task kept time. */
void robohub_core_scheduler_note_run(robohub_core_task_id id, rh_u32 now_ms);
robohub_core_task_id robohub_core_scheduler_late_task(rh_u32 now_ms, rh_u32 watchdog_ms);
/* With an operating system beneath, each task becomes a thread of its own. */
RoboHubError robohub_core_scheduler_start_threads(void);

/* Applications. */
void robohub_core_app_reset(void);
RoboHubError robohub_core_app_init(void);
RoboHubError robohub_core_app_start(void);
void robohub_core_app_step(rh_u32 now_ms);
void robohub_core_app_stop(void);
/* With an operating system beneath, one thread publishes the tick and steps the
 * applications; it is the runtime's own loop, moved off the caller's thread. */
RoboHubError robohub_core_app_start_thread(void);

/* Health (RNODE-HEALTH-0001..0003): read at boot before any application, the
 * watchdog started when the loop begins and serviced on every pass. */
void robohub_core_health_boot(void);
void robohub_core_watchdog_begin(void);
void robohub_core_watchdog_service(rh_u32 now_ms);
void robohub_core_health_reset(void);
/* The period begin uses: RH_CORE_WATCHDOG_MS unless a test says otherwise. */
void robohub_core_watchdog_configure(rh_u32 period_ms);

#endif /* ROBOHUB_CORE_INTERNAL_H */
