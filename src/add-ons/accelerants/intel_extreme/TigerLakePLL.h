/*
 * Copyright 2024, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef TIGERLAKEPLL_H
#define TIGERLAKEPLL_H

#include "intel_extreme.h"

#include <SupportDefs.h>


bool ComputeHdmiDpll(int freq, int* Pdiv, int* Qdiv, int* Kdiv, uint32* bestdco);
bool ComputeDisplayPortDpll(int freq, int* Pdiv, int* Qdiv, int* Kdiv, uint32* bestdco);

status_t ProgramPLL(int which, int Pdiv, int Qdiv, int Kdiv, uint32 dcoKhz, uint32 ddiClockOff);


#endif /* !TIGERLAKEPLL_H */
