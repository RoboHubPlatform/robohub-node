/**
 * @file sensors.c
 * @brief The template's default sensor set: none (RNODE-TPL-0001).
 *
 * Mesh replaces this file when it builds a node from a plan, with one block per
 * fitted module. Without one the node still links, reports its health and takes
 * updates over its link; it publishes no readings.
 *
 * @project RoboHub Platform
 * @license MIT
 */

#include "sensors.h"

void sensors_init(void)
{
}

void sensors_poll(void)
{
}

RoboHubError sensors_last_error(void)
{
    return ROBOHUB_OK;
}
