/*
 * Copyright 2026 Akakor. All rights reserved.
 * Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef RTWN_SDIO_VAR_H
#define RTWN_SDIO_VAR_H


#include <sys/taskqueue.h>

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>


struct sdio_func;
struct rtw_pwr_seq_cmd;


/* Large enough for the whole receive FIFO of the RTL8723BS (16 KiB). */
#define RTWN_SDIO_RXBUFSZ	(16 * 1024 + RTWN_SDIO_BLOCK_SIZE)
/* One frame with its descriptor, rounded up to whole blocks. */
#define RTWN_SDIO_TXBUFSZ	(4 * 1024)

/* Transmit FIFOs, in the order of RTWN_SDIO_ADDR_TXFF_*. */
enum {
	RTWN_SDIO_TXQ_HIGH,
	RTWN_SDIO_TXQ_NORMAL,
	RTWN_SDIO_TXQ_LOW,
	RTWN_SDIO_TXQ_COUNT
};

/* Failed commands in a row after which a card counts as not answering. */
#define RTWN_SDIO_MAX_IO_ERRORS	8
/* Failed accesses that are logged, per device. */
#define RTWN_SDIO_MAX_IO_LOGS	100

struct rtwn_sdio_softc {
	struct rtwn_softc	sc_sc;		/* must be the first */
	struct sdio_func	*sc_func;

	bool			sc_powered;	/* CMD53 works on MAC registers */
	bool			sc_intr_enabled;
	uint32_t		sc_himr;

	int			sc_io_errors_logged;
	uint8_t			sc_rx_id;	/* low bits of the RX FIFO address */

	/* Looks for receive requests the card interrupt missed. */
	thread_id		sc_poll_thread;
	volatile bool		sc_poll_stop;

	uint8_t			*sc_rx_buf;
	uint8_t			*sc_tx_buf;

	/* Frames written to the chip, completed by sc_tx_task. */
	struct mbufq		sc_tx_done;
	struct task		sc_tx_task;
};
#define RTWN_SDIO_SOFTC(sc)	((struct rtwn_sdio_softc *)(sc))


/* rtwn_sdio_reg.c */
uint8_t		rtwn_sdio_read_1(struct rtwn_softc *, uint16_t);
uint16_t	rtwn_sdio_read_2(struct rtwn_softc *, uint16_t);
uint32_t	rtwn_sdio_read_4(struct rtwn_softc *, uint16_t);
int		rtwn_sdio_write_1(struct rtwn_softc *, uint16_t, uint8_t);
int		rtwn_sdio_write_2(struct rtwn_softc *, uint16_t, uint16_t);
int		rtwn_sdio_write_4(struct rtwn_softc *, uint16_t, uint32_t);
void		rtwn_sdio_delay(struct rtwn_softc *, int);
int		rtwn_sdio_fw_write_block(struct rtwn_softc *, const uint8_t *,
		    uint16_t, int);

uint8_t		rtwn_sdio_local_read_1(struct rtwn_sdio_softc *, uint16_t);
uint16_t	rtwn_sdio_local_read_2(struct rtwn_sdio_softc *, uint16_t);
uint32_t	rtwn_sdio_local_read_4(struct rtwn_sdio_softc *, uint16_t);
void		rtwn_sdio_io_fault(struct rtwn_sdio_softc *);
int		rtwn_sdio_local_write_1(struct rtwn_sdio_softc *, uint16_t,
		    uint8_t);
int		rtwn_sdio_local_write_4(struct rtwn_sdio_softc *, uint16_t,
		    uint32_t);

int		rtwn_sdio_run_pwr_seq(struct rtwn_sdio_softc *,
		    const struct rtw_pwr_seq_cmd * const *);
int		rtwn_sdio_resume_hci(struct rtwn_sdio_softc *);
size_t		rtwn_sdio_align_size(size_t);

/* rtwn_sdio_rx.c */
void		rtwn_sdio_service(void *);
void		rtwn_sdio_enable_intr(struct rtwn_sdio_softc *, bool);
void		rtwn_sdio_poll_start(struct rtwn_sdio_softc *);
void		rtwn_sdio_poll_stop(struct rtwn_sdio_softc *);

/* rtwn_sdio_tx.c */
int		rtwn_sdio_tx_start(struct rtwn_softc *, struct ieee80211_node *,
		    struct mbuf *, uint8_t *, uint8_t, int);
void		rtwn_sdio_tx_done_task(void *, int);
void		rtwn_sdio_tx_drain(struct rtwn_sdio_softc *, int);

/* rtl8723b/sdio/r8723bs_attach.c */
int		r8723bs_attach(struct rtwn_sdio_softc *);

#endif	/* RTWN_SDIO_VAR_H */
