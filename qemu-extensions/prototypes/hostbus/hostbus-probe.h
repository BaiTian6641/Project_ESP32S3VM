/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Controlled host-dependency experiment; not an ESP32-S3 peripheral model. */
#ifndef ESP32S3VM_HOSTBUS_PROBE_H
#define ESP32S3VM_HOSTBUS_PROBE_H

#include <stdbool.h>

/* Called under the BQL before vm_prepare_start changes ticks/state/events. */
bool esp32s3vm_hostbus_resume_blocked(void);
/* Restore is rejected whenever an external-peer probe object is present. */
bool esp32s3vm_hostbus_present(void);

#endif
