/**
 * @file keystore_double.h
 * @brief The host tests' keystore (keystore_double.c).
 *
 * @project RoboHub Platform
 * SPDX-License-Identifier: MIT
 */

#ifndef TESTS_KEYSTORE_DOUBLE_H
#define TESTS_KEYSTORE_DOUBLE_H

#include "robohub/keystore.h"

extern int keystore_double_fail;   /* every call answers -1, as with no store */
extern int keystore_double_stores; /* successful stores so far */
extern int keystore_double_has;    /* a record is held */
extern rh_key_t keystore_double_key;

void keystore_double_reset(void);

#endif
