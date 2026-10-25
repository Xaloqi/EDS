// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Xaloqi
/*
 * =============================================================================
 * Xaloqi EDS
 * FILE: platform/zephyr/eds_partition.h
 *
 * PURPOSE: One name for "device / offset / size of the flash partition whose
 *          DTS node label is <label>", on every supported Zephyr line [#516].
 *
 *          Zephyr 4.4 renamed FIXED_PARTITION_DEVICE/OFFSET/SIZE/ID to
 *          PARTITION_DEVICE/OFFSET/SIZE/ID and marked the old names
 *          __DEPRECATED_MACRO (an error with warnings as errors). Zephyr 3.7
 *          has only the old names. The expansion is the same on both sides
 *          (DT_REG_ADDR / DT_REG_SIZE / the partition's flash controller), so
 *          the offsets and sizes an example passes to nvm_store do not change.
 *
 *          The version gate has the form used by zephyr_can.c (#513).
 *
 * USAGE  : EDS_PARTITION_DEVICE(eds_nvs_slot), EDS_PARTITION_OFFSET(...),
 *          EDS_PARTITION_SIZE(...), EDS_PARTITION_ID(image_1). The argument is the DTS node label, as for
 *          the Zephyr macros.
 * =============================================================================
 */
#ifndef EDS_PARTITION_H
#define EDS_PARTITION_H

#include <zephyr/storage/flash_map.h>
#include <zephyr/version.h>

#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 4, 0)
#define EDS_PARTITION_DEVICE(label) PARTITION_DEVICE(label)
#define EDS_PARTITION_OFFSET(label) PARTITION_OFFSET(label)
#define EDS_PARTITION_SIZE(label)   PARTITION_SIZE(label)
#define EDS_PARTITION_ID(label)     PARTITION_ID(label)
#else
#define EDS_PARTITION_DEVICE(label) FIXED_PARTITION_DEVICE(label)
#define EDS_PARTITION_OFFSET(label) FIXED_PARTITION_OFFSET(label)
#define EDS_PARTITION_SIZE(label)   FIXED_PARTITION_SIZE(label)
#define EDS_PARTITION_ID(label)     FIXED_PARTITION_ID(label)
#endif

#endif /* EDS_PARTITION_H */
