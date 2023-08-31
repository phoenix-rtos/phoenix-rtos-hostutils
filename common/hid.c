/*
 * Phoenix-RTOS
 *
 * hid - HID functions for psu
 *
 * Copyright 2019 Phoenix Systems
 * Author: Bartosz Ciesla, Aleksander Kaminski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include "hostutils-common/hid.h"


hid_device *open_device(uint16_t vid, uint16_t pid)
{
	hid_device *h = NULL;
	struct hid_device_info *list = hid_enumerate(vid, pid), *it;

	for (it = list; it != NULL; it = it->next) {
		if ((h = hid_open_path(it->path)) != NULL)
			break;
	}

	if (list != NULL)
		hid_free_enumeration(list);

	return h;
}

