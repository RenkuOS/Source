/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */


/*!	Register access of rtwn over SDIO.

	MAC registers are reached through function 1 at RTWN_SDIO_MAC(), the
	SDIO local registers at RTWN_SDIO_LOCAL(). One byte always goes through
	CMD52, and so does every access to the SDIO registers below 0x100: the
	vendor driver does the same (sdio_read32 in sdio_ops.c), and CMD53
	reads of HISR and FREE_TXPG fail now and then (70 of them in the log of
	build #172). 16 bit accesses are always made byte by byte: a 2 byte
	CMD53 fails on the RTL8723BS (the rtw88 driver does the same, see
	sdio.c there). After the chip is powered on, the other aligned 32 bit
	accesses use one CMD53.
*/


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/time.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>

#include <dev/sdio/sdio_haiku.h>

#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>
#include <dev/rtwn/sdio/rtwn_sdio_var.h>

#include <dev/rtwn/rtl8723b/r8723b_pwrseq.h>


static uint32_t
rtwn_sdio_read_bytewise(struct rtwn_sdio_softc *uc, uint32_t addr, int len,
    int *err)
{
	uint32_t value = 0;
	int i;

	for (i = 0; i < len; i++) {
		uint8_t byte = sdio_read_1(uc->sc_func, addr + i, err);
		if (*err != 0)
			return (0xffffffff);
		value |= (uint32_t)byte << (i * 8);
	}
	return (value);
}


static int
rtwn_sdio_write_bytewise(struct rtwn_sdio_softc *uc, uint32_t addr,
    uint32_t value, int len)
{
	int i, err = 0;

	for (i = 0; i < len; i++) {
		sdio_write_1(uc->sc_func, addr + i, (value >> (i * 8)) & 0xff,
		    &err);
		if (err != 0)
			return (err);
	}
	return (0);
}


/*!	A block transfer (CMD53) failed: let the watchdog check that the chip
	still receives (sc_io_fault in if_rtwnvar.h).
*/
void
rtwn_sdio_io_fault(struct rtwn_sdio_softc *uc)
{
	struct rtwn_softc *sc = &uc->sc_sc;

	if (sc->sc_io_fault == 0)
		sc->sc_io_fault = 1;
}


static uint32_t
rtwn_sdio_read(struct rtwn_sdio_softc *uc, uint32_t addr, int len)
{
	int err = 0;
	uint32_t value;
	bool bytewise = len != 4 || addr < RTWN_SDIO_ADDR(1, 0)
	    || !uc->sc_powered || (addr % 4) != 0;

	if (len == 1)
		value = sdio_read_1(uc->sc_func, addr, &err);
	else if (bytewise)
		value = rtwn_sdio_read_bytewise(uc, addr, len, &err);
	else
		value = sdio_read_4(uc->sc_func, addr, &err);

	if (err != 0) {
		if (!bytewise && len == 4)
			rtwn_sdio_io_fault(uc);
		/* Keep the log readable when the card stops answering. */
		if (uc->sc_io_errors_logged++ < RTWN_SDIO_MAX_IO_LOGS)
			device_printf(uc->sc_sc.sc_dev,
			    "read%d at %#x failed: %d\n", len * 8, addr, err);
		return (0xffffffff);
	}

	return (value);
}


static int
rtwn_sdio_write(struct rtwn_sdio_softc *uc, uint32_t addr, uint32_t value,
    int len)
{
	int err = 0;
	bool bytewise = len != 4 || addr < RTWN_SDIO_ADDR(1, 0)
	    || !uc->sc_powered || (addr % 4) != 0;

	if (len == 1)
		sdio_write_1(uc->sc_func, addr, value, &err);
	else if (bytewise)
		err = rtwn_sdio_write_bytewise(uc, addr, value, len);
	else
		sdio_write_4(uc->sc_func, addr, value, &err);

	if (err != 0 && !bytewise && len == 4)
		rtwn_sdio_io_fault(uc);
	if (err != 0 && uc->sc_io_errors_logged++ < RTWN_SDIO_MAX_IO_LOGS) {
		device_printf(uc->sc_sc.sc_dev, "write%d at %#x failed: %d\n",
		    len * 8, addr, err);
	}

	return (err);
}


uint8_t
rtwn_sdio_read_1(struct rtwn_softc *sc, uint16_t addr)
{
	return (rtwn_sdio_read(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), 1));
}


uint16_t
rtwn_sdio_read_2(struct rtwn_softc *sc, uint16_t addr)
{
	return (rtwn_sdio_read(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), 2));
}


uint32_t
rtwn_sdio_read_4(struct rtwn_softc *sc, uint16_t addr)
{
	return (rtwn_sdio_read(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), 4));
}


int
rtwn_sdio_write_1(struct rtwn_softc *sc, uint16_t addr, uint8_t value)
{
	return (rtwn_sdio_write(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), value,
	    1));
}


int
rtwn_sdio_write_2(struct rtwn_softc *sc, uint16_t addr, uint16_t value)
{
	return (rtwn_sdio_write(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), value,
	    2));
}


int
rtwn_sdio_write_4(struct rtwn_softc *sc, uint16_t addr, uint32_t value)
{
	return (rtwn_sdio_write(RTWN_SDIO_SOFTC(sc), RTWN_SDIO_MAC(addr), value,
	    4));
}


uint8_t
rtwn_sdio_local_read_1(struct rtwn_sdio_softc *uc, uint16_t reg)
{
	return (rtwn_sdio_read(uc, RTWN_SDIO_LOCAL(reg), 1));
}


uint16_t
rtwn_sdio_local_read_2(struct rtwn_sdio_softc *uc, uint16_t reg)
{
	return (rtwn_sdio_read(uc, RTWN_SDIO_LOCAL(reg), 2));
}


uint32_t
rtwn_sdio_local_read_4(struct rtwn_sdio_softc *uc, uint16_t reg)
{
	return (rtwn_sdio_read(uc, RTWN_SDIO_LOCAL(reg), 4));
}


int
rtwn_sdio_local_write_1(struct rtwn_sdio_softc *uc, uint16_t reg,
    uint8_t value)
{
	return (rtwn_sdio_write(uc, RTWN_SDIO_LOCAL(reg), value, 1));
}


int
rtwn_sdio_local_write_4(struct rtwn_sdio_softc *uc, uint16_t reg,
    uint32_t value)
{
	return (rtwn_sdio_write(uc, RTWN_SDIO_LOCAL(reg), value, 4));
}


void
rtwn_sdio_delay(struct rtwn_softc *sc, int usec)
{
	if (usec < 1000)
		DELAY(usec);
	else
		pause("rtwn", MAX(1, (int)USEC_2_TICKS(usec)));
}


/*!	The firmware is written to its window at 0x1000 one dword at a time, as
	the rtw88 driver does for the RTL8723BS.
*/
int
rtwn_sdio_fw_write_block(struct rtwn_softc *sc, const uint8_t *buf,
    uint16_t reg, int len)
{
	struct rtwn_sdio_softc *uc = RTWN_SDIO_SOFTC(sc);
	int error;

	while (len > 0) {
		int chunk = MIN(len, 4);
		uint32_t value = 0;
		int i;

		for (i = 0; i < chunk; i++)
			value |= (uint32_t)buf[i] << (i * 8);

		if (chunk == 4 && uc->sc_powered && (reg % 4) == 0) {
			sdio_write_4(uc->sc_func, RTWN_SDIO_MAC(reg), value,
			    &error);
		} else {
			error = rtwn_sdio_write_bytewise(uc, RTWN_SDIO_MAC(reg),
			    value, chunk);
		}
		if (error != 0)
			return (error);

		buf += chunk;
		reg += chunk;
		len -= chunk;
	}
	return (0);
}


/*!	A CMD53 transfer must be one command: up to a block it is rounded to
	dwords, above that to whole blocks (like sdio_align_size() of Linux).
*/
size_t
rtwn_sdio_align_size(size_t size)
{
	size = roundup2(size, 4);
	if (size > RTWN_SDIO_BLOCK_SIZE)
		size = roundup2(size, RTWN_SDIO_BLOCK_SIZE);
	return (size);
}


/*!	Takes the SDIO interface of the chip out of suspend. */
int
rtwn_sdio_resume_hci(struct rtwn_sdio_softc *uc)
{
	uint8_t value;
	int tries, errors = 0;

	value = rtwn_sdio_local_read_1(uc, RTWN_SDIO_HSUS_CTRL);
	rtwn_sdio_local_write_1(uc, RTWN_SDIO_HSUS_CTRL,
	    value & ~RTWN_SDIO_HSUS_SUS_REQ);

	for (tries = 0; tries < RTW_PWR_POLLING_CNT; tries++) {
		int err = 0;

		value = sdio_read_1(uc->sc_func,
		    RTWN_SDIO_LOCAL(RTWN_SDIO_HSUS_CTRL), &err);
		if (err != 0) {
			/* A card that does not answer will not get ready. */
			if (++errors >= RTWN_SDIO_MAX_IO_ERRORS)
				return (EIO);
		} else {
			errors = 0;
			if ((value & RTWN_SDIO_HSUS_RESUME_RDY) != 0)
				return (0);
		}
		DELAY(50);
	}

	device_printf(uc->sc_sc.sc_dev,
	    "SDIO does not leave suspend (HSUS_CTRL %#x)\n", value);
	return (ETIMEDOUT);
}


/*!	Runs the steps of a power sequence (the format of rtw88, see
	r8723b_pwrseq.h) that apply to SDIO.
*/
int
rtwn_sdio_run_pwr_seq(struct rtwn_sdio_softc *uc,
    const struct rtw_pwr_seq_cmd * const *seq)
{
	const struct rtw_pwr_seq_cmd *cmd;
	int i, err;

	for (i = 0; seq[i] != NULL; i++) {
		for (cmd = seq[i]; cmd->cmd != RTW_PWR_CMD_END; cmd++) {
			uint32_t addr;
			uint8_t value;
			int tries, errors;

			if ((cmd->intf_mask & RTW_PWR_INTF_SDIO_MSK) == 0
			    || cmd->cut_mask == RTW_PWR_CUT_TEST_MSK)
				continue;

			addr = cmd->base == RTW_PWR_ADDR_SDIO
			    ? RTWN_SDIO_LOCAL(cmd->offset)
			    : RTWN_SDIO_MAC(cmd->offset);

			switch (cmd->cmd) {
			case RTW_PWR_CMD_WRITE:
				err = 0;
				value = sdio_read_1(uc->sc_func, addr, &err);
				if (err == 0) {
					value = (value & ~cmd->mask)
					    | (cmd->value & cmd->mask);
					sdio_write_1(uc->sc_func, addr, value,
					    &err);
				}
				if (err != 0)
					return (err);
				break;

			case RTW_PWR_CMD_POLLING:
				/*
				 * Polling is for a chip that is not ready yet,
				 * not for one that does not answer: each failed
				 * command takes its timeout, and 20000 of them
				 * with the lock held stall a shutdown.
				 */
				errors = 0;
				for (tries = 0; tries < RTW_PWR_POLLING_CNT;
				    tries++) {
					err = 0;
					value = sdio_read_1(uc->sc_func, addr,
					    &err);
					if (err == 0 && (value & cmd->mask)
					    == (cmd->value & cmd->mask))
						break;
					if (err == 0)
						errors = 0;
					else if (++errors
					    >= RTWN_SDIO_MAX_IO_ERRORS) {
						device_printf(uc->sc_sc.sc_dev,
						    "power sequence %d: no "
						    "answer polling %#x\n", i,
						    cmd->offset);
						return (EIO);
					}
					DELAY(50);
				}
				if (tries == RTW_PWR_POLLING_CNT) {
					device_printf(uc->sc_sc.sc_dev,
					    "power sequence %d: polling %#x "
					    "for %#x/%#x timed out\n", i,
					    cmd->offset, cmd->value,
					    cmd->mask);
					return (ETIMEDOUT);
				}
				break;

			case RTW_PWR_CMD_DELAY:
				if (cmd->value == RTW_PWR_DELAY_US)
					DELAY(cmd->offset);
				else
					DELAY(cmd->offset * 1000);
				break;

			default:
				break;
			}
		}
	}

	return (0);
}
