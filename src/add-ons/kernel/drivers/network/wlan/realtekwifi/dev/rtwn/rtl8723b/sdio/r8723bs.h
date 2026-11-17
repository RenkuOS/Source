/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef RTL8723BS_H
#define RTL8723BS_H

#include <dev/rtwn/rtl8723b/r8723b.h>

/*
 * Function declarations.
 */
/* r8723bs_init.c */
int	r8723bs_power_on(struct rtwn_softc *);
void	r8723bs_power_off(struct rtwn_softc *);
void	r8723bs_post_init(struct rtwn_softc *);

#endif	/* RTL8723BS_H */
