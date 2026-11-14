/* SPDX-License-Identifier: MIT */
#include "linux/types.h"

#ifndef BRCMS_LINUX_INTERRUPT_H
#define BRCMS_LINUX_INTERRUPT_H

// The bottom half the Linux driver defers its interrupt work to. Nothing in
// the phy layer reaches it.
struct tasklet_struct {
	int unused;
};

#endif
