#include "hid.h"


hid_device *open_device(uint16_t vid, uint16_t pid)
{
    hid_device* h = NULL;
	struct hid_device_info* list = hid_enumerate(vid, pid);

	for (struct hid_device_info* it = list; it != NULL; it = it->next) {
		if ((h = hid_open_path(it->path)) == NULL) {
			continue;
		} else {
			break;
		}
	}

	if (list)
		hid_free_enumeration(list);

	return h;
}

