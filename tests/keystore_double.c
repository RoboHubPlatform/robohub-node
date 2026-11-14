/**
 * @file keystore_double.c
 * @brief A keystore for host tests: one record in memory, and a switch that
 * makes it fail like a platform with no store.
 *
 * The real stores are the platforms' (robohub/keystore.h); these strong
 * definitions stand in for them, and for the weak defaults, in the tests.
 *
 * @project RoboHub Platform
 * SPDX-License-Identifier: MIT
 */

#include "keystore_double.h"

#include <string.h>

int keystore_double_fail = 0;
int keystore_double_stores = 0;
int keystore_double_has = 0;
rh_key_t keystore_double_key;

int rh_keystore_load(rh_key_t* out)
{
    if (keystore_double_fail || !keystore_double_has || out == 0) {
        return -1;
    }
    *out = keystore_double_key;
    return 0;
}

int rh_keystore_store(const rh_key_t* key)
{
    if (keystore_double_fail || key == 0) {
        return -1;
    }
    keystore_double_key = *key;
    keystore_double_has = 1;
    keystore_double_stores++;
    return 0;
}

void keystore_double_reset(void)
{
    keystore_double_fail = 0;
    keystore_double_stores = 0;
    keystore_double_has = 0;
    memset(&keystore_double_key, 0, sizeof(keystore_double_key));
}
