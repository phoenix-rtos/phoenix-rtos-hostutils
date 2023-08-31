/*
 * Phoenix-RTOS
 *
 * hid - HID functions for psu
 *
 * Copyright 2019 Phoenix Systems
 * Author: Bartosz Ciesla
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _HID_H_
#define _HID_H_

#include <stdint.h>
#include <hidapi/hidapi.h>

extern hid_device *open_device(uint16_t vid, uint16_t pid);

#endif
