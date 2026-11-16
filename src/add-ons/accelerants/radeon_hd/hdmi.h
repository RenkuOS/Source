/*
 * Copyright 2026, Kevin Adams, kevinadams05@gmail.com. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *		Kevin Adams, kevinadams05@gmail.com
 */
#ifndef RADEON_HD_HDMI_H
#define RADEON_HD_HDMI_H


#include <Accelerant.h>
#include <SupportDefs.h>


void hdmi_avi_infoframe_program(uint8 crtcID, display_mode* mode);
void hdmi_registers_dump(uint8 crtcID);


#endif /* RADEON_HD_HDMI_H */
