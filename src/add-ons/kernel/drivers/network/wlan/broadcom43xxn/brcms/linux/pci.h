/* SPDX-License-Identifier: MIT */
#include "linux/types.h"

// Upstream this header drags the DMA mapping calls in with it, and the
// imported sources rely on that.
#include "linux/dma-mapping.h"
