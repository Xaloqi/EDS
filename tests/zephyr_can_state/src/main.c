// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * EDS#513: the CAN state-change (bus-off) handler of platform/zephyr/zephyr_can.c.
 *
 * Zephyr's fake CAN driver stands in for the controller. A state change is
 * delivered the way a CAN driver delivers it: through
 * can_fire_state_change_callbacks() on Zephyr >= 4.5; on older Zephyr the fake
 * driver records the one callback it was given, and the test calls that.
 */
#include <zephyr/ztest.h>
#include <zephyr/fff.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/can/can_fake.h>
#include <zephyr/version.h>

#include "can_transport.h"
#include "zephyr_can.h"

#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 5, 0)
#define STATE_CB_LIST 1
#endif

/* The fake CAN driver is built on fff, which needs its globals defined once. */
DEFINE_FFF_GLOBALS;

static const struct device *const can = DEVICE_DT_GET(DT_ALIAS(can0));

static void deliver(enum can_state state)
{
	struct can_bus_err_cnt ec = {0};

#ifdef STATE_CB_LIST
	can_fire_state_change_callbacks(can, state, ec);
#else
	can_state_change_callback_t cb = fake_can_set_state_change_callback_fake.arg1_val;

	zassert_not_null(cb, "no state change callback registered");
	cb(can, state, ec, fake_can_set_state_change_callback_fake.arg2_val);
#endif
}

static bool bus_off(can_transport_t *t)
{
	bool off = false;

	zassert_equal(can_transport_get_status(t, &off), UDS_STATUS_OK);
	return off;
}

ZTEST(eds_can_state, test_handler_registered_and_sets_bus_off)
{
	can_transport_t *t = NULL;
	TC_PRINT("Zephyr %s, ZEPHYR_VERSION_CODE %#x, state change callbacks: %s\n",
		 KERNEL_VERSION_STRING, ZEPHYR_VERSION_CODE,
		 IS_ENABLED(STATE_CB_LIST) ? "list (4.5)" : "single (<= 4.4)");

	zassert_true(device_is_ready(can));
	zassert_equal(zephyr_can_platform_init(&t, can, 0U), UDS_STATUS_OK);
	zassert_not_null(t);

#ifdef STATE_CB_LIST
	struct can_driver_data *d = can->data;

	zassert_false(sys_slist_is_empty(&d->state_change_callbacks), "handler not registered");
#else
	zassert_equal(fake_can_set_state_change_callback_fake.call_count, 1U);
#endif

	zassert_false(bus_off(t), "bus-off flag must start cleared");

	deliver(CAN_STATE_ERROR_PASSIVE);
	zassert_false(bus_off(t), "error-passive is not bus-off");

	deliver(CAN_STATE_BUS_OFF);
	zassert_true(bus_off(t), "bus-off state must set the flag");

	deliver(CAN_STATE_ERROR_ACTIVE);
	zassert_false(bus_off(t), "leaving bus-off must clear the flag");

	/* Re-initialising must not register the handler twice or lose it. */
	zassert_equal(zephyr_can_platform_init(&t, can, 0U), UDS_STATUS_OK);
	deliver(CAN_STATE_BUS_OFF);
	zassert_true(bus_off(t));
}

ZTEST_SUITE(eds_can_state, NULL, NULL, NULL, NULL, NULL);
