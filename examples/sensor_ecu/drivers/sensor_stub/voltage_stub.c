/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu/drivers/sensor_stub/voltage_stub.c
 *
 * PURPOSE: [EDS#339] Minimal native_sim-only Zephyr sensor driver backing
 *          the "xaloqi,voltage-stub" devicetree binding. Returns a fixed
 *          nominal reading (12.000 V) -- see ambient_temp_stub.c's header
 *          comment for why a fixed value is sufficient and deliberate.
 *          Implements only sample_fetch/channel_get -- no triggers, no
 *          power management, one channel. Not meant to be used outside
 *          this example.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#define DT_DRV_COMPAT xaloqi_voltage_stub

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

/** Fixed nominal reading: 12.000 V. See file header -- intentionally static. */
#define VOLTAGE_STUB_VAL1 (12)
#define VOLTAGE_STUB_VAL2 (0)

struct voltage_stub_data {
	int32_t val1;
	int32_t val2;
};

static int voltage_stub_sample_fetch(const struct device *dev,
				      enum sensor_channel chan)
{
	ARG_UNUSED(dev);

	if ((chan != SENSOR_CHAN_VOLTAGE) && (chan != SENSOR_CHAN_ALL)) {
		return -ENOTSUP;
	}

	/* Fixed reading -- nothing to sample. */
	return 0;
}

static int voltage_stub_channel_get(const struct device *dev,
				     enum sensor_channel chan,
				     struct sensor_value *val)
{
	const struct voltage_stub_data *data = dev->data;

	if (chan != SENSOR_CHAN_VOLTAGE) {
		return -ENOTSUP;
	}

	val->val1 = data->val1;
	val->val2 = data->val2;

	return 0;
}

static const struct sensor_driver_api voltage_stub_api = {
	.sample_fetch = voltage_stub_sample_fetch,
	.channel_get  = voltage_stub_channel_get,
};

static int voltage_stub_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define VOLTAGE_STUB_INIT(inst)                                               \
	static struct voltage_stub_data voltage_stub_data_##inst = {          \
		.val1 = VOLTAGE_STUB_VAL1,                                     \
		.val2 = VOLTAGE_STUB_VAL2,                                     \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(inst, voltage_stub_init, NULL,                   \
			      &voltage_stub_data_##inst, NULL,                 \
			      POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,       \
			      &voltage_stub_api);

DT_INST_FOREACH_STATUS_OKAY(VOLTAGE_STUB_INIT)
