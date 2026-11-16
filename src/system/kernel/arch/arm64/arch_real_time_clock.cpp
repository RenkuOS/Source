/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <arch/real_time_clock.h>

#include <real_time_clock.h>
#include <real_time_data.h>
#include <smp.h>


// There is no RTC driver: the boot loader reads the EFI clock once, and the
// clock counts on from there with system_time().
static uint64 sBootSeconds;
static bigtime_t sBootSystemTime;


status_t
arch_rtc_init(kernel_args *args, struct real_time_data *data)
{
	sBootSeconds = args->rtc_boot_seconds;
	sBootSystemTime = system_time();
		// Later than the loader's GetTime(), so the clock starts that much
		// behind (the time spent in ExitBootServices() and early kernel init).
	if (sBootSeconds == 0)
		dprintf("arch_rtc_init: no time from the firmware, starting at 1970\n");
	return B_OK;
}


uint64
arch_rtc_get_hw_time(void)
{
	if (sBootSeconds == 0)
		return 0;
	return sBootSeconds + (system_time() - sBootSystemTime) / 1000000;
}


void
arch_rtc_set_hw_time(uint64 seconds)
{
	// The kernel doesn't call EFI runtime services (the loader passes it no
	// system table), so there is no SetTime() and the clock isn't written.
}


void
arch_rtc_set_system_time_offset(struct real_time_data *data, bigtime_t offset)
{
	atomic_set64(&data->arch_data.system_time_offset, offset);
}


bigtime_t
arch_rtc_get_system_time_offset(struct real_time_data *data)
{
	return atomic_get64(&data->arch_data.system_time_offset);
}
