/* SPDX-License-Identifier: MIT */
#include "linux/types.h"

#ifndef BRCMS_LINUX_WORKQUEUE_H
#define BRCMS_LINUX_WORKQUEUE_H

struct delayed_work {
	timer	haiku_timer;
	bool	armed;
};

#endif
