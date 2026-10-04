/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: examples/sensor_ecu/drivers/sensor_stub/ambient_temp_stub.c
 *
 * PURPOSE: [EDS#339] Minimal native_sim-only Zephyr sensor driver backing
 *          the "xaloqi,ambient-temp-stub" devicetree binding. Returns a
 *          fixed nominal reading (25 degC) -- there is no physical sensor
 *          to simulate on native_sim, and no dynamic value is needed: DTC
 *          firing/clearing is demonstrated by writing extreme thresholds
 *          via DID 0xD010/0xD011 (see diagnostics_config.yaml's
 *          calibration_write / dtc_clear_and_verify jobs), not by varying
 *          the reading. Implements only sample_fetch/channel_get -- no
 *          triggers, no power management, one channel. This exists so
 *          sensor_monitor.c's real sensor_sample_fetch()/
 *          sensor_channel_get() calls have an actual struct device to
 *          resolve against; it is deliberately not a production driver
 *          and is not meant to be used outside this example. On real
 *          hardware, replace the devicetree node this binds to with a
 *          physical sensor driver -- see native_sim.overlay's header
 *          comment for examples.
 *
 * SPDX-License-Identifier: Apache-2.0
 * =============================================================================
 */

#define DT_DRV_COMPAT xaloqi_ambient_temp_stub

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

/** Fixed nominal reading: 25.000 degC. See file header -- intentionally static. */
#define AMBIENT_TEMP_STUB_MILLI_C (25000)

struct ambient_temp_stub_data {
	int32_t milli_c;
};

static int ambient_temp_stub_sample_fetch(const struct device *dev,
					   enum sensor_channel chan)
{
	ARG_UNUSED(dev);

	if ((chan != SENSOR_CHAN_AMBIENT_TEMP) && (chan != SENSOR_CHAN_ALL)) {
		return -ENOTSUP;
	}

	/* Fixed reading -- nothing to sample. */
	return 0;
}

static int ambient_temp_stub_channel_get(const struct device *dev,
					  enum sensor_channel chan,
					  struct sensor_value *val)
{
	const struct ambient_temp_stub_data *data = dev->data;

	if (chan != SENSOR_CHAN_AMBIENT_TEMP) {
		return -ENOTSUP;
	}

	val->val1 = data->milli_c / 1000;
	val->val2 = (data->milli_c % 1000) * 1000;

	return 0;
}

static const struct sensor_driver_api ambient_temp_stub_api = {
	.sample_fetch = ambient_temp_stub_sample_fetch,
	.channel_get  = ambient_temp_stub_channel_get,
};

static int ambient_temp_stub_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define AMBIENT_TEMP_STUB_INIT(inst)                                          \
	static struct ambient_temp_stub_data ambient_temp_stub_data_##inst = {\
		.milli_c = AMBIENT_TEMP_STUB_MILLI_C,                          \
	};                                                                     \
	DEVICE_DT_INST_DEFINE(inst, ambient_temp_stub_init, NULL,              \
			      &ambient_temp_stub_data_##inst, NULL,            \
			      POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY,       \
			      &ambient_temp_stub_api);

DT_INST_FOREACH_STATUS_OKAY(AMBIENT_TEMP_STUB_INIT)
