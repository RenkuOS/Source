/*
 * Copyright 2026, rainygirl <rainygirl@gmail.com>.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 */

/*!	Starting the imported phy layer.

	brcmsmac reaches the phy through a chain of its own structures, built
	up by brcms_c_attach() and brcms_b_attach() across some two thousand
	lines that are mostly about mac80211, the transmit rings and the rate
	tables. None of that is needed to bring the radio up, but the fields
	the phy layer reads out of those structures are, so they are filled
	here in the same order brcms_b_attach() fills them, and then the
	imported brcms_b_init() is called, which is what runs the phy's own
	initialisation and calibration.
*/


#include "linux/types.h"

#include <KernelExport.h>
#include <FindDirectory.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "net/mac80211.h"
#include "linux/ieee80211.h"
#include "linux/bcma/bcma.h"
#include "aiutils.h"
#include "brcm_hw_ids.h"
#include "brcmu_utils.h"
#include "chipcommon.h"
#include "d11.h"
#include "dma.h"
#include "debug_haiku.h"
#include "glue.h"
#include "glue_net.h"
#include "mac80211_if.h"
#include "main.h"
#include "phy/phy_hal.h"
#include "phy_shim.h"
#include "pub.h"
#include "soc.h"
#include "types.h"
#include "ucode_loader.h"


#define TRACE(x...)	dprintf("broadcom43xxn: " x)

// Where the firmware this driver needs is kept: a copy in non-packaged
// overrides the one the haiku package installs. The two files are the
// microcode and a table saying what is where inside it.
#define FIRMWARE_SUBDIRECTORY	"firmware/broadcom43xxn"

static const directory_which kFirmwareDirectories[] = {
	B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
	B_SYSTEM_DATA_DIRECTORY
};

#define VENDOR_BROADCOM		0x14e4

// From brcmsmac's main.c: keep the core flags the band already worked out,
// and the contention window a band starts with.
#define BRCMS_USE_COREFLAGS	0xffffffff
#define APHY_CWMIN			15
#define PHY_CWMAX			1023


/*!	One tagged part of the firmware file, as its companion table lists it. */
struct firmware_hdr {
	__le32	offset;
	__le32	len;
	__le32	idx;
};


struct firmware {
	size_t		size;
	const u8*	data;
};


// Everything brcmsmac hangs off itself, in one piece: there is one card.
static struct brcms_info sInfo;
static struct brcms_c_info sCommon;
static struct brcms_pub sPublic;
static struct brcms_hardware sHardware;
static struct brcms_hw_band sHardwareBands[MAXBANDS];
static struct brcms_band sBands[MAXBANDS];
static struct brcms_core sCore;
static struct brcms_stf sSingleStreamFilter;

// The network this card is on, as brcmsmac keeps it. Nothing here fills it
// in -- net80211 keeps that -- but it is read in places this driver does
// not reach yet, and a structure that is there and empty is safer than a
// pointer that is not there at all.
static struct brcms_bss_cfg sNetwork;
static struct firmware sMicrocode;
static struct firmware sMicrocodeTable;
static struct wiphy sWiphy;


static status_t
load_firmware(const char* name, struct firmware* out)
{
	char path[B_PATH_NAME_LENGTH];
	int file = B_ENTRY_NOT_FOUND;
	for (size_t i = 0; i < B_COUNT_OF(kFirmwareDirectories) && file < 0;
			i++) {
		char directory[B_PATH_NAME_LENGTH];
		if (find_directory(kFirmwareDirectories[i], -1, false, directory,
				sizeof(directory)) != B_OK) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/" FIRMWARE_SUBDIRECTORY "/%s",
			directory, name);
		file = open(path, O_RDONLY);
	}
	if (file < 0) {
		TRACE("firmware: cannot find %s\n", name);
		return file;
	}

	off_t size = lseek(file, 0, SEEK_END);
	lseek(file, 0, SEEK_SET);
	if (size <= 0) {
		close(file);
		return B_BAD_DATA;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(file);
		return B_NO_MEMORY;
	}

	ssize_t bytes = read(file, data, size);
	close(file);
	if (bytes != size) {
		free(data);
		return B_IO_ERROR;
	}

	out->data = data;
	out->size = size;

	return B_OK;
}


/* --- the firmware accessors, from brcmsmac's mac80211_if.c --- */

int
brcms_ucode_init_buf(struct brcms_info* wl, void** pbuf, u32 idx)
{
	int i, entry;
	const u8 *pdata;
	struct firmware_hdr *hdr;
	for (i = 0; i < wl->fw.fw_cnt; i++) {
		hdr = (struct firmware_hdr *)wl->fw.fw_hdr[i]->data;
		for (entry = 0; entry < wl->fw.hdr_num_entries[i];
		     entry++, hdr++) {
			u32 len = le32_to_cpu(hdr->len);
			if (le32_to_cpu(hdr->idx) == idx) {
				pdata = wl->fw.fw_bin[i]->data +
					le32_to_cpu(hdr->offset);
				*pbuf = kvmemdup(pdata, len, GFP_KERNEL);
				if (*pbuf == NULL)
					return -ENOMEM;
				return 0;
			}
		}
	}
	TRACE("firmware: tag %u is not in the table\n", (unsigned)idx);
	*pbuf = NULL;
	return -ENODATA;
}


int
brcms_ucode_init_uint(struct brcms_info* wl, size_t* n_bytes, u32 idx)
{
	int i, entry;
	const u8 *pdata;
	struct firmware_hdr *hdr;
	for (i = 0; i < wl->fw.fw_cnt; i++) {
		hdr = (struct firmware_hdr *)wl->fw.fw_hdr[i]->data;
		for (entry = 0; entry < wl->fw.hdr_num_entries[i];
		     entry++, hdr++) {
			if (le32_to_cpu(hdr->idx) == idx) {
				pdata = wl->fw.fw_bin[i]->data +
					le32_to_cpu(hdr->offset);
				if (le32_to_cpu(hdr->len) != 4)
					return -ENOMSG;
				*n_bytes = le32_to_cpu(*((__le32 *) pdata));
				return 0;
			}
		}
	}
	TRACE("firmware: tag %u is not in the table\n", (unsigned)idx);
	return -ENOMSG;
}


void
brcms_ucode_free_buf(void* p)
{
	kvfree(p);
}


int
brcms_check_firmwares(struct brcms_info* wl)
{
	if (wl->fw.fw_bin[0] == NULL || wl->fw.fw_hdr[0] == NULL)
		return -EBADF;
	if ((wl->fw.fw_hdr[0]->size % sizeof(struct firmware_hdr)) != 0)
		return -EBADF;
	if (wl->fw.fw_bin[0]->size < MIN_FW_SIZE
		|| wl->fw.fw_bin[0]->size > MAX_FW_SIZE) {
		return -EBADF;
	}

	return 0;
}


// The driver callbacks the imported hardware code makes that belong to the
// stack above it, which this driver does not have yet.
void brcms_init(struct brcms_info* wl) {}
uint brcms_reset(struct brcms_info* wl) { return 0; }
void brcms_fatal_error(struct brcms_info* wl) { TRACE("fatal error\n"); }
bool brcms_rfkill_set_hw_state(struct brcms_info* wl) { return false; }


/*!	Fills in what brcms_b_attach() would, and attaches the phy.

	The order is brcms_b_attach()'s: the interconnect first, because the
	chip has to say what it is before anything else can be decided; then
	the core out of reset; then the board, out of the SPROM; and only then
	the phy, which reads all of it back out of struct shared_phy.
*/
static status_t
attach(void)
{
	struct bcma_device* core = brcms_glue_d11core();
	struct bcma_bus* bus = brcms_glue_bus();
	struct shared_phy_params shared;
	int i;

	sInfo.pub = &sPublic;
	sInfo.wlc = &sCommon;
	sInfo.wiphy = &sWiphy;

	sCommon.pub = &sPublic;
	sCommon.wl = &sInfo;
	sCommon.hw = &sHardware;
	sCommon.wiphy = &sWiphy;
	sCommon.core = &sCore;
	sCommon.stf = &sSingleStreamFilter;

	// How many aerials there are to send and receive with, which this card
	// has two of each. brcms_c_stf_phy_chain_calc() works these out from
	// the SPROM and falls back to these same values for an N-PHY; nothing
	// here called it, so every one of them read zero.
	//
	// Zero streams is not merely wrong, it is quiet: brcms_c_set_ratetable()
	// passes txstreams to brcms_c_rateset_mcs_upd(), which clears every
	// scheme at or above it. With zero, the whole MCS half of the table the
	// microcode answers a frame from was being wiped, so an 802.11n frame
	// arrived and nothing went back.
	sSingleStreamFilter.hw_txchain = TXCHAIN_DEF_NPHY;
	sSingleStreamFilter.txchain = TXCHAIN_DEF_NPHY;
	sSingleStreamFilter.txstreams = 2;
	sSingleStreamFilter.hw_rxchain = RXCHAIN_DEF_NPHY;
	sSingleStreamFilter.rxchain = RXCHAIN_DEF_NPHY;
	sSingleStreamFilter.rxstreams = 2;

	// Which aerial to transmit on, as a choice rather than a command.
	// Zero is ANT_TX_FORCE_0 -- "aerial nought, and no other" -- which is
	// not what an unset field was meant to say. brcmsmac sets this to
	// ANT_TX_DEF in its own attach.
	sSingleStreamFilter.txant = ANT_TX_DEF;

	// Which kind of frame a modulation and coding scheme makes. frametype()
	// returns this for anything that is one, and zero here would say CCK,
	// which is not something an HT frame can be sent as.
	sCommon.mimoft = FT_HT;
	sCommon.bsscfg = &sNetwork;
	sNetwork.wlc = &sCommon;
	sSingleStreamFilter.phytxant = PHY_TXC_ANT_0_1;
	// Automatic ACK/CTS responses use the hardware copy, independently
	// of the antenna bits in software-built transmit descriptors.
	sHardware.bmac_phytxant = sSingleStreamFilter.phytxant;
	for (i = 0; i < MAXBANDS; i++) {
		sCommon.bandstate[i] = &sBands[i];
		sHardware.bandstate[i] = &sHardwareBands[i];
	}
	sCommon.band = sCommon.bandstate[0];

	sHardware.wlc = &sCommon;
	sHardware.unit = 0;
	sHardware.band = sHardware.bandstate[0];
	sHardware._piomode = true;

	brcms_b_info_init(&sHardware);

	sHardware.sih = ai_attach(bus);
	if (sHardware.sih == NULL) {
		TRACE("attach: the interconnect would not answer\n");
		return B_ERROR;
	}

	sHardware.vendorid = VENDOR_BROADCOM;
	sHardware.deviceid = bus->boardinfo.type;
	sHardware.d11core = core;
	sHardware.corerev = core->id.rev;

	TRACE("attach: chip %#x rev %u package %u, d11 rev %u\n",
		ai_get_chip_id(sHardware.sih), ai_get_chiprev(sHardware.sih),
		ai_get_chippkg(sHardware.sih), sHardware.corerev);

	ai_clkctl_init(sHardware.sih);
	sCommon.fastpwrup_dly = ai_clkctl_fast_pwrup_delay(sHardware.sih);

	brcms_b_clkctl_clk(&sHardware, BCMA_CLKMODE_FAST);
	brcms_b_corereset(&sHardware, BRCMS_USE_COREFLAGS);

	if (!brcms_b_validate_chip_access(&sHardware)) {
		TRACE("attach: the core does not read back what was written\n");
		return B_ERROR;
	}

	sHardware.boardrev = bus->sprom.board_rev;
	if (sHardware.boardrev == BOARDREV_PROMOTABLE)
		sHardware.boardrev = BOARDREV_PROMOTED;
	sHardware.sromrev = bus->sprom.revision;
	memcpy(sHardware.etheraddr, bus->sprom.il0mac, ETH_ALEN);
	sHardware.boardflags = bus->sprom.boardflags_lo
		+ (bus->sprom.boardflags_hi << 16);
	sHardware.boardflags2 = bus->sprom.boardflags2_lo
		+ (bus->sprom.boardflags2_hi << 16);

	// This part carries both bands. The 2.4 GHz one is band zero.
	sHardware._nbands = 2;

	sPublic.sih = sHardware.sih;
	sPublic.corerev = sHardware.corerev;
	sPublic.sromrev = sHardware.sromrev;
	sPublic.boardrev = sHardware.boardrev;
	sPublic.boardflags = sHardware.boardflags;
	sPublic.boardflags2 = sHardware.boardflags2;
	sPublic._nbands = sHardware._nbands;
	sPublic.ieee_hw = NULL;

	sHardware.physhim = wlc_phy_shim_attach(&sHardware, &sInfo, &sCommon);
	if (sHardware.physhim == NULL)
		return B_NO_MEMORY;

	shared.sih = sHardware.sih;
	shared.physhim = sHardware.physhim;
	shared.unit = 0;
	shared.corerev = sHardware.corerev;
	shared.vid = sHardware.vendorid;
	shared.did = sHardware.deviceid;
	shared.chip = ai_get_chip_id(sHardware.sih);
	shared.chiprev = ai_get_chiprev(sHardware.sih);
	shared.chippkg = ai_get_chippkg(sHardware.sih);
	shared.sromrev = sHardware.sromrev;
	shared.boardtype = ai_get_boardtype(sHardware.sih);
	shared.boardrev = sHardware.boardrev;
	shared.boardflags = sHardware.boardflags;
	shared.boardflags2 = sHardware.boardflags2;

	sHardware.phy_sh = wlc_phy_shared_attach(&shared);
	if (sHardware.phy_sh == NULL)
		return B_NO_MEMORY;

	// Each band in turn, which is how brcmsmac does it: band zero is
	// always 2.4 GHz and band one, where the part has it, is 5 GHz. Each
	// gets a phy of its own, because what the radio has to be told
	// differs between them.
	uint bands = brcms_glue_settings()->band5 ? sHardware._nbands : 1;

	sHardware.xmtfifo_sz = brcms_glue_xmtfifo_sz(sHardware.corerev);
	if (sHardware.xmtfifo_sz == NULL) {
		TRACE("attach: no fifo sizes for core revision %u\n",
			sHardware.corerev);
		return B_NOT_SUPPORTED;
	}

	sCommon.core->coreidx = core->core_index;

	sHardware.machwcap = bcma_read32(core, D11REGOFFS(machwcap));
	sHardware.machwcap_backup = sHardware.machwcap;

	// The descriptor rings have to exist before brcms_b_init() runs:
	// brcms_b_coreinit() initialises the receive ring without testing it
	// first, because upstream always attaches them here.
	if (brcms_glue_dma_init() != B_OK)
		return B_NO_MEMORY;

	if (!brcms_b_attach_dmapio(&sCommon, 0, true)) {
		TRACE("attach: the descriptor rings would not come up\n");
		return B_NO_MEMORY;
	}

	for (uint j = 0; j < bands; j++) {
		brcms_c_setxband(&sHardware, j);

		sHardware.band->bandunit = j;
		sHardware.band->bandtype = j != 0 ? BRCM_BAND_5G : BRCM_BAND_2G;
		sCommon.band->bandunit = j;
		sCommon.band->bandtype = sHardware.band->bandtype;

		sHardware.band->CWmin = APHY_CWMIN;
		sHardware.band->CWmax = PHY_CWMAX;

		sHardware.band->pi = wlc_phy_attach(sHardware.phy_sh, core,
			sHardware.band->bandtype, &sWiphy);
		if (sHardware.band->pi == NULL) {
			TRACE("attach: the phy would not attach for band %u\n", j);
			return B_ERROR;
		}

		// PHY calibration uses its own hardware chain masks. Leaving
		// hw_phytxchain at zero forces both RF switches to receive in
		// the N-PHY transmit-chain workaround.
		wlc_phy_stf_chain_init(sHardware.band->pi,
			sSingleStreamFilter.hw_txchain, sSingleStreamFilter.hw_rxchain);
		TRACE("attach: PHY chains tx %#x rx %#x\n",
			sSingleStreamFilter.hw_txchain, sSingleStreamFilter.hw_rxchain);

		wlc_phy_machwcap_set(sHardware.band->pi, sHardware.machwcap);
		wlc_phy_get_phyversion(sHardware.band->pi,
			&sHardware.band->phytype, &sHardware.band->phyrev,
			&sHardware.band->radioid, &sHardware.band->radiorev);
		sHardware.band->abgphy_encore
			= wlc_phy_get_encore(sHardware.band->pi);
		sCommon.band->abgphy_encore = sHardware.band->abgphy_encore;
		sHardware.band->core_flags
			= wlc_phy_get_coreflags(sHardware.band->pi);

		sCommon.band->pi = sHardware.band->pi;
		sCommon.band->phytype = sHardware.band->phytype;
		sCommon.band->phyrev = sHardware.band->phyrev;
		sCommon.band->radioid = sHardware.band->radioid;
		sCommon.band->radiorev = sHardware.band->radiorev;

		TRACE("attach: band %u is phy type %u rev %u, radio %#x rev %u\n",
			j, sHardware.band->phytype, sHardware.band->phyrev,
			sHardware.band->radioid, sHardware.band->radiorev);

		if (!BRCMS_ISNPHY(sHardware.band)) {
			TRACE("attach: not an n-phy, nothing here drives it\n");
			return B_NOT_SUPPORTED;
		}
	}

	// Back to the band everything else starts on.
	brcms_c_setxband(&sHardware, 0);

	sPublic.phy_11ncapable = true;
	sHardware.phyclk = true;
	sHardware.clk = true;

	return B_OK;
}


status_t
brcms_glue_phy_init(uint16 chanspec)
{
	status_t status = load_firmware("bcm43xx-0.fw", &sMicrocode);
	if (status != B_OK)
		return status;
	status = load_firmware("bcm43xx_hdr-0.fw", &sMicrocodeTable);
	if (status != B_OK)
		return status;

	sInfo.fw.fw_bin[0] = &sMicrocode;
	sInfo.fw.fw_hdr[0] = &sMicrocodeTable;
	sInfo.fw.hdr_num_entries[0]
		= sMicrocodeTable.size / sizeof(struct firmware_hdr);
	sInfo.fw.fw_cnt = 1;

	TRACE("firmware: %lu bytes, %u table entries\n",
		(unsigned long)sMicrocode.size, sInfo.fw.hdr_num_entries[0]);

	status = brcms_glue_start_timer_thread();
	if (status != B_OK)
		return status;

	status = attach();
	if (status != B_OK)
		return status;

	int error = brcms_ucode_data_init(&sInfo, &sInfo.ucode);
	if (error < 0) {
		TRACE("phy_init: the firmware table is not what was expected "
			"(%d)\n", error);
		return B_BAD_DATA;
	}

	TRACE("phy_init: microcode %lu bytes\n",
		(unsigned long)sInfo.ucode.bcm43xx_16_mimosz);

	// Bringing the hardware up comes first, and the last thing it does is
	// reset the d11 core. Anything put in before that is thrown away, so
	// brcmsmac does this and only then initialises: brcms_c_up() calls
	// brcms_b_up_prep(), brcms_b_up_finish(), and then brcms_c_init(),
	// which is what calls brcms_b_init().
	if (brcms_b_up_prep(&sHardware) != 0) {
		TRACE("phy_init: the hardware would not come up\n");
		return B_ERROR;
	}

	brcms_b_up_finish(&sHardware);

	// Now the microcode goes in, the core is set up, and the phy runs its
	// own initialisation and calibration against the channel.
	brcms_b_init(&sHardware, chanspec);

	return B_OK;
}


/*!	Opens the receiver and listens.

	brcms_b_init() leaves the card initialised but deaf: the MAC is
	suspended, the rings are empty and nothing is being taken off the air.
	This fills the receive ring, tells the microcode to hand up everything
	it hears rather than only what is addressed here, and lets the MAC run.

	Nothing is transmitted. The point is to find out whether the receive
	chain works, and the quietest way to ask is to listen for the beacons
	every network in the room is sending anyway.
*/
/*!	How many times the MAC was found switched off during a listen. */
static uint32 sMacWentQuiet;


/*!	The state everything else works through.

	One card, one of each of these, and the transmit and receive sides are
	separate files that both have to reach them.
*/
struct brcms_hardware*
brcms_glue_hardware(void)
{
	return &sHardware;
}


struct brcms_c_info*
brcms_glue_common(void)
{
	return &sCommon;
}


/*!	What the card says about itself, rather than what it was told.

	The control word says whether the MAC is running and what it has been
	asked to keep; the debug word is the microcode's own idea of what it is
	doing, which is 2 once it is listening and 3 while it is suspended.
*/
/*!	What the chip itself counted.

	Its own tally of what arrived and what it did about it, which is the
	only account that covers the frames this driver never sees: a frame
	whose preamble the phy could not read is not a frame with a bad check
	sequence, it is a frame that was never there as far as anything above
	the phy is concerned.
*/
void
brcms_glue_chip_report(void)
{
	struct macstat stats;

	if (sHardware.d11core == NULL || !sHardware.clk)
		return;

	brcms_b_copyfrom_objmem(&sHardware, M_UCODE_MACSTAT, &stats,
		sizeof(stats), OBJADDR_SHM_SEL);

	TRACE("chip: %u receptions started, %u bad preamble, %u bad check, "
		"%u glitches\n", stats.rxstrt, stats.rxbadplcp, stats.rxbadfcs,
		stats.rxcrsglitch);
	TRACE("chip: %u acks sent, %u acks heard, %u frames sent, %u to us, "
		"%u to others\n", stats.txackfrm, stats.rxackucast,
		stats.txallfrm, stats.rxdfrmucastmbss, stats.rxdfrmocast);

	TRACE("chip: %u RX FIFO overflows, %u BE TX FIFO underflows\n",
		stats.rxf0ovfl, stats.txfunfl[TX_AC_BE_FIFO]);

	struct dma_pub* rxDma = sHardware.di[RX_FIFO];
	if (rxDma != NULL)
		TRACE("dma: RX ready max %u, ring exhausted %u, no buffer %u, giants %u\n",
			rxDma->rxreadymax, rxDma->rxringfull, rxDma->rxnobuf, rxDma->rxgiants);

	// What the phy says is stopping it keeping itself in tune: the low
	// half is what is holding it, then whether its watchdog is allowed to
	// run at all, whether the periodic calibration is switched off, and
	// which calibration mode it is in.
	if (sHardware.band != NULL && sHardware.band->pi != NULL) {
		u8 target = 0, lowest = 0, highest = 0;

		TRACE("chip: phy holds %#lx\n",
			(unsigned long)wlc_phy_holds_get(sHardware.band->pi));

		wlc_phy_power_report(sHardware.band->pi, &target, &lowest,
			&highest);
		TRACE("chip: CCK[0] target %u.%u dBm, table min/max %u.%u to "
			"%u.%u\n", target / 4, (target % 4) * 25, lowest / 4,
			(lowest % 4) * 25, highest / 4, (highest % 4) * 25);
	}
}


void
brcms_glue_mac_report(void)
{
	struct bcma_device* d11 = sHardware.d11core;
	if (d11 == NULL)
		return;

	TRACE("mac: maccontrol %#lx, macintstatus %#lx, ucode state %u, "
		"suspend depth %u, stopped %" B_PRIu32 " times\n",
		(unsigned long)bcma_read32(d11, D11REGOFFS(maccontrol)),
		(unsigned long)bcma_read32(d11, D11REGOFFS(macintstatus)),
		brcms_b_read_shm(&sHardware, M_UCODE_DBGST),
		sHardware.mac_suspend_depth, sMacWentQuiet);
}


/*!	How much power the transmitter may use on the band it is on now.

	Upstream works this out in its channel manager, from what the regulator
	allows and what the board says it can do, and hands it to the phy;
	nothing here does, and a phy that has never been told has a target of
	zero, which transmits into nothing and does not say so.

	What the board says is as far as this goes: the SPROM's maximum for
	the band, in quarter dB, for every rate. Each band has a phy of its
	own and a figure of its own, so this is said again after crossing from
	one to the other -- a radio that has just been started on the quiet
	band has been told nothing at all.
*/
void
brcms_glue_set_power_limit(void)
{
	struct txpwr_limits limits;
	const struct ssb_sprom* sprom = &brcms_glue_bus()->sprom;
	bool quiet_band = sHardware.band->bandtype == BRCM_BAND_5G;

	if (sHardware.band == NULL || sHardware.band->pi == NULL)
		return;

	// The quiet band is written down in three stretches, because what the
	// aerial manages at the bottom of it is not what it manages at the
	// top.
	u8 board;

	if (quiet_band) {
		uint channel = CHSPEC_CHANNEL(
			wlc_phy_chanspec_get(sHardware.band->pi));

		if (channel <= 48)
			board = sprom->core_pwr_info[0].maxpwr_5gl;
		else if (channel >= 149)
			board = sprom->core_pwr_info[0].maxpwr_5gh;
		else
			board = sprom->core_pwr_info[0].maxpwr_5g;
	} else
		board = sprom->core_pwr_info[0].maxpwr_2g;

	// A board that says nothing for a band is not a board that may be
	// driven at nothing.
	if (board == 0 || board == 0xff)
		board = 68;

	memset(&limits, board, sizeof(limits));

	wlc_phy_txpower_limit_set(sHardware.band->pi, &limits,
		wlc_phy_chanspec_get(sHardware.band->pi));

	TRACE("power: band %u limited to %u.%u dBm\n",
		sHardware.band->bandunit, board / 4, (board % 4) * 25);
}


status_t
brcms_glue_listen(void)
{
	if (sHardware.band == NULL || sHardware.band->pi == NULL)
		return B_NO_INIT;

	TRACE("listen: starting\n");

	brcms_glue_rx_reset();

	// Everything below is written with the MAC held still, which is how
	// brcms_c_init() writes the same registers. One suspend covers all of
	// it: brcms_c_suspend_mac_and_wait() counts rather than looking at the
	// chip, so the phy's own suspends inside wlc_phy_txpower_limit_set()
	// and brcms_c_mac_promisc() nest into this one and return at once
	// instead of waiting 83 ms for a microcode that has already answered.
	//
	// The count is taken from the chip rather than trusted. A suspend that
	// timed out, or a phy path whose release was not taken, leaves it
	// reading high, and the MAC would then never be let go.
	uint32 control = bcma_read32(sHardware.d11core, D11REGOFFS(maccontrol));
	TRACE("listen: maccontrol %#" B_PRIx32 ", suspend depth %u\n", control,
		sHardware.mac_suspend_depth);

	if ((control & MCTL_EN_MAC) != 0) {
		sHardware.mac_suspend_depth = 0;
		brcms_c_suspend_mac_and_wait(&sCommon);
	} else {
		// Stopped already, by brcms_glue_quiet() or by the phy. Say so,
		// so that the release at the end of this is the one that counts.
		sHardware.mac_suspend_depth = 1;
	}

	// Which aerials the phy is to use, for sending and for hearing.
	// Nothing ever told it: brcmsmac does this from
	// brcms_c_stf_phy_chain_upd() and this driver reached neither. A phy
	// left to its own idea of its aerials hears less than the card can,
	// and an access point that is not heard clearly answers slowly.
	if (brcms_glue_settings()->stf_chain) {
		wlc_phy_stf_chain_set(sHardware.band->pi,
			sSingleStreamFilter.txchain, sSingleStreamFilter.rxchain);
		TRACE("listen: aerials tx %#x rx %#x\n",
			sSingleStreamFilter.txchain, sSingleStreamFilter.rxchain);
	}

	TRACE("listen: mac held, setting the transmit power\n");

	brcms_glue_set_power_limit();

	// The engine has to be allowed to fetch from memory before any of the
	// rings mean anything. brcms_b_up_prep() turned this on already; this
	// is here because nothing else says so out loud.
	brcms_glue_set_bus_master(true);

	// Who this card is, as far as the microcode is concerned. Promiscuous
	// reception does not depend on it, but brcms_c_init() writes it and
	// leaving the match registers at whatever the reset left behind is not
	// a state anything was tested in.
	brcms_b_set_addrmatch(&sHardware, RCM_MAC_OFFSET, sHardware.etheraddr);

	TRACE("listen: this card is %02x:%02x:%02x:%02x:%02x:%02x\n",
		sHardware.etheraddr[0], sHardware.etheraddr[1],
		sHardware.etheraddr[2], sHardware.etheraddr[3],
		sHardware.etheraddr[4], sHardware.etheraddr[5]);

	// Hand up every beacon and probe response, from this network or any
	// other. Without this the microcode drops everything not addressed
	// here, which at this point is everything.
	brcms_c_mac_promisc(&sCommon,
		FIF_BCN_PRBRESP_PROMISC | FIF_OTHER_BSS | FIF_PROBE_REQ);

	// Which rate the microcode is to answer a frame at. It reads this from
	// shared memory, and nothing had ever written it: an access point that
	// sent quickly got no acknowledgement it could hear, backed off, and
	// settled on 802.11b. Every frame arriving here was CCK, and sixty
	// four megabytes took three and a half minutes.
	if (brcms_glue_settings()->basic_rates)
		brcms_glue_init_rate_lookup();

	// What the microcode answers an aggregate with, and how long it is
	// prepared to wait for one.
	if (brcms_glue_settings()->ba_template) {
		brcms_glue_write_ba_template();
		brcms_glue_write_ampdu_shm();
	}

	if (brcms_glue_settings()->rate_table)
		brcms_glue_write_rate_tables();

	// How the MAC is to contend for the air. brcms_c_init() does this and
	// nothing here did, so until now the chip has been backing off by
	// whatever its reset left behind. It is set while the MAC is still
	// suspended, which is where brcmsmac does it too.
	bcma_set16(sHardware.d11core, D11REGOFFS(ifs_ctl), IFS_USEEDCF);
	brcms_c_edcf_setparams(&sCommon, false);
	for (uint fifo = 0; fifo < 4; fifo++) {
		uint base = M_EDCF_QINFO + fifo * M_EDCF_QLEN;
		TRACE("listen: queue %u aifs %u cwmin %u cwmax %u\n", fifo,
			brcms_b_read_shm(&sHardware, base + 8),
			brcms_b_read_shm(&sHardware, base + 2),
			brcms_b_read_shm(&sHardware, base + 4));
	}

	// And the microcode's own switch for it, which is a separate thing
	// from the sequencer's. The four queues were written above and the
	// register was told to use them, but the microcode was never told it
	// was running an 802.11e medium at all -- and a microcode that is not
	// is a microcode that answers a run of aggregated frames with nothing,
	// because a block acknowledgement belongs to a traffic stream and
	// there are no traffic streams without this. An access point whose
	// aggregates go unanswered works its way down to 802.11b and stays
	// there, which is what this card has been hearing.
	//
	// brcmsmac sets it in brcms_c_up(), for both bands at once.
	if (brcms_glue_settings()->edcf) {
		brcms_b_mhf(&sHardware, MHF1, MHF1_EDCF, MHF1_EDCF,
			BRCM_BAND_ALL);
	}

	// The release matching the suspend at the top. Every register this
	// function writes is in place by now.
	brcms_c_enable_mac(&sCommon);

	TRACE("listen: response antenna control %#x (cached %#x)\n",
		brcms_b_read_shm(&sHardware, M_RSP_PCTLWD),
		sHardware.bmac_phytxant);

	TRACE("listen: receiver open on chanspec %#x\n",
		wlc_phy_chanspec_get(sHardware.band->pi));

	brcms_glue_mac_report();

	// How many frame buffers the ring took says whether it was filled at
	// all. An empty ring hears nothing and does not say so.
	brcms_glue_dma_report();

	return B_OK;
}


/*!	Takes whatever the card has heard off the ring.

	There is no interrupt handler yet, so the ring is read out by asking
	rather than being told. brcms_b_recv() walks the descriptors, turns
	each one back into a frame and hands it to brcms_c_recv().

	It is asked for a bounded pass. Unbounded, it loops for as long as the
	ring says there is more, and a ring that says so wrongly never lets go.
*/
bool
brcms_glue_poll(void)
{
	// Something still suspends the MAC behind this driver's back. Rather
	// than listen to a card that is not running, notice and put it back,
	// and count how often so that it is not mistaken for silence.
	struct bcma_device* d11 = sHardware.d11core;
	if ((bcma_read32(d11, D11REGOFFS(maccontrol)) & MCTL_EN_MAC) == 0) {
		sMacWentQuiet++;
		sHardware.mac_suspend_depth = 1;
		brcms_c_enable_mac(&sCommon);
	}

	return brcms_b_recv(&sHardware, RX_FIFO, true);
}


uint32
brcms_glue_mac_stops(void)
{
	return sMacWentQuiet;
}


void
brcms_glue_phy_uninit(void)
{
	// Nothing may be armed while the rest of this runs, and nothing may
	// arm itself again afterwards.
	brcms_glue_quiesce_timers();

	// Cancellation stops timer interrupts, but a PHY callback may already
	// be running on the worker. Join it before disabling hardware or
	// freeing the PHY and its calibration timer.
	brcms_glue_stop_timer_thread();

	brcms_glue_quiesce_hardware(&sHardware);

	// The phy's own detach is what frees its calibration timer, so it has
	// to see the list before the sweep below empties it.
	if (sHardware.band != NULL && sHardware.band->pi != NULL) {
		wlc_phy_detach(sHardware.band->pi);
		sHardware.band->pi = NULL;
	}

	brcms_glue_stop_timers();

	brcms_glue_dma_uninit();

	brcms_ucode_data_free(&sInfo.ucode);

	free((void*)sMicrocode.data);
	free((void*)sMicrocodeTable.data);
	sMicrocode.data = NULL;
	sMicrocodeTable.data = NULL;
}
