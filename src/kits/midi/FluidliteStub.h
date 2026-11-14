/*
 * Copyright 2026, The RenkuOS authors.
 * SPDX-License-Identifier: MIT
 */
#ifndef _FLUIDLITE_STUB_H
#define _FLUIDLITE_STUB_H

/*
	Stands in for <fluidlite.h> on architectures that have no fluidlite
	package. new_fluid_synth() fails, so BSoftSynth::InitCheck() returns
	false and the rest of the Midi Kit builds and runs without a synth.
	Only the calls SoftSynth.cpp makes are declared here.
 */

#include <stddef.h>


typedef struct _fluid_settings_t fluid_settings_t;
typedef struct _fluid_synth_t fluid_synth_t;


static inline fluid_settings_t*
new_fluid_settings()
{
	return NULL;
}


static inline void
delete_fluid_settings(fluid_settings_t*)
{
}


static inline int
fluid_settings_setnum(fluid_settings_t*, const char*, double)
{
	return 0;
}


static inline int
fluid_settings_setint(fluid_settings_t*, const char*, int)
{
	return 0;
}


static inline fluid_synth_t*
new_fluid_synth(fluid_settings_t*)
{
	return NULL;
}


static inline int
delete_fluid_synth(fluid_synth_t*)
{
	return 0;
}


static inline int
fluid_synth_sfload(fluid_synth_t*, const char*, int)
{
	return -1;
}


static inline void
fluid_synth_set_gain(fluid_synth_t*, float)
{
}


static inline float
fluid_synth_get_gain(fluid_synth_t*)
{
	return 0.0f;
}


static inline void
fluid_synth_set_reverb_on(fluid_synth_t*, int)
{
}


static inline void
fluid_synth_set_reverb(fluid_synth_t*, double, double, double, double)
{
}


static inline int
fluid_synth_noteon(fluid_synth_t*, int, int, int)
{
	return -1;
}


static inline int
fluid_synth_noteoff(fluid_synth_t*, int, int)
{
	return -1;
}


static inline int
fluid_synth_cc(fluid_synth_t*, int, int, int)
{
	return -1;
}


static inline int
fluid_synth_program_change(fluid_synth_t*, int, int)
{
	return -1;
}


static inline int
fluid_synth_pitch_bend(fluid_synth_t*, int, int)
{
	return -1;
}


static inline int
fluid_synth_write_float(fluid_synth_t*, int, void*, int, int, void*, int,
	int)
{
	return -1;
}


#endif // _FLUIDLITE_STUB_H
