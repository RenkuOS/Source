/*
 * Copyright 2019-2020, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */
#ifndef _MMC_H
#define _MMC_H


#include <device_manager.h>


#define MMC_BUS_MODULE_NAME "bus_managers/mmc/driver_v1"


struct IOOperation;


// Bus timings, from slowest to fastest. The clock follows the timing:
// 25MHz for the legacy timing, 52MHz (or the closest lower value) otherwise.
typedef enum mmc_bus_timing {
	MMC_TIMING_LEGACY = 0,
		// Default after initialization, single data rate
	MMC_TIMING_HS,
		// High speed, single data rate: the controller's "high speed enable"
		// bit
	MMC_TIMING_HS_SDR25,
		// Same, but also selecting the UHS SDR25 mode, which some controllers
		// (Intel) need for high speed
	MMC_TIMING_DDR52,
		// High speed, dual data rate (UHS DDR50 mode). eMMC only, needs a 4
		// or 8 bit bus.
	MMC_TIMING_HS200,
		// 200MHz single data rate (UHS SDR104 mode), eMMC only, 1.8V
		// signaling, 4 or 8 bit bus. The controller runs the tuning procedure.
	MMC_TIMING_HS400,
		// 200MHz dual data rate, eMMC only, 8 bit bus. Only reachable from a
		// tuned HS200 bus (the tuning is kept). Not in the SD host controller
		// specification: uses the Intel UHS mode 5.
} mmc_bus_timing;


static inline const char*
mmc_timing_name(int timing)
{
	static const char* const kNames[] = { "legacy", "HS52", "HS52 (SDR25)",
		"DDR52", "HS200", "HS400" };
	if (timing < MMC_TIMING_LEGACY || timing > MMC_TIMING_HS400)
		return "?";
	return kNames[timing];
}


// Flags for set_timing()
#define MMC_TIMING_SIGNALING_1V8	0x01	// set the 1.8V signaling enable bit
#define MMC_TIMING_KEEP_TUNING		0x02	// don't reset the tuning result
#define MMC_TIMING_NO_VERIFY		0x04	// don't check transfers afterwards
#define MMC_TIMING_QUERY			0x08	// only check that the controller
											// supports the timing, change nothing

typedef enum card_type {
	CARD_TYPE_UNKNOWN,
	CARD_TYPE_MMC,
	CARD_TYPE_MMC_EXTENDED_CAPACITY,
	CARD_TYPE_SD,
	CARD_TYPE_SDHC,
	CARD_TYPE_UHS1,
	CARD_TYPE_UHS2,
	CARD_TYPE_SDIO
} card_type;


// Commands for SD/eMMC cards defined in:
// SD: Physical Layer Simplified Specification Version 8.00
// eMMC: JEDEC Standard No. 84-B51. Sec 6.10.4
//
// They are in the common .h file for the mmc stack because the SDHCI driver
// currently needs to map them to the corresponding expected response types.
//
// TODO maybe it would be simpler to have the upper layers (the bus manager and mmc_disk) specify
// the expected response type when sending a command. This would avoid hardcoding command types
// and the corresponding response types in the command sending code.
//
// When the commands are specific to SD or MMC types, the constant is named with the corresponding
// prefix.
enum SD_COMMANDS {
	// Basic commands, class 0
	GO_IDLE_STATE = 0,
	MMC_SEND_OP_COND = 1,
	ALL_SEND_CID = 2,
	SD_SEND_RELATIVE_ADDR = 3,
	MMC_SET_RELATIVE_ADDR = 3,
	MMC_SWITCH = 6, // Same command number as SD_SET_BUS_WIDTH, but this one is not an ACMD
	SELECT_DESELECT_CARD = 7,
	SD_SEND_IF_COND = 8,
	MMC_SEND_EXT_CSD = 8,
	SEND_CSD = 9,
	SD_STOP_TRANSMISSION = 12,
	SEND_STATUS = 13,

	// Block oriented read and write commands, class 2
	SD_READ_SINGLE_BLOCK = 17,
	SD_READ_MULTIPLE_BLOCKS = 18,
	MMC_SEND_TUNING_BLOCK = 21, // HS200 tuning, reads a known pattern
	SET_BLOCK_COUNT = 23,
		// Mandatory for eMMC, optional for SD (see CMD_SUPPORT in the SCR)

	SD_WRITE_SINGLE_BLOCK = 24,
	SD_WRITE_MULTIPLE_BLOCKS = 25,

	// Erase commands, class 5
	SD_ERASE_WR_BLK_START = 32,
	SD_ERASE_WR_BLK_END = 33,
	SD_ERASE = 38,

	// Application specific commands, class 8
	SD_APP_CMD = 55,

	// I/O mode commands, class 9 (SDIO Simplified Specification 3.00,
	// section 5)
	SDIO_IO_SEND_OP_COND = 5,
	SDIO_IO_RW_DIRECT = 52,
	SD_IO_ABORT = 52,
		// old name of SDIO_IO_RW_DIRECT
	SDIO_IO_RW_EXTENDED = 53,
};


// Application commands are sent with a prefix CMD55 (SD_APP_CMD)
enum SDHCI_APPLICATION_COMMANDS {
	SD_SET_BUS_WIDTH = 6,
	SD_SEND_OP_COND = 41,
};


// Interface between mmc_bus and underlying implementation (sdhci_pci or any
// other thing that can execute mmc commands)
typedef struct mmc_bus_interface {
	driver_module_info info;

	status_t (*set_clock)(void* controller, uint32_t kilohertz);
		// Configure the bus clock. The bus is initialized with a slow clock
		// that allows device enumeration in all cases, but after enumeration
		// the mmc_bus knows how fast each card can go, and configures the bus
		// accordingly.
	status_t (*execute_command)(void* controller, uint8_t command,
		uint32_t argument, uint32_t* result);
		// Execute a command with no I/O phase
	status_t (*do_io)(void* controller, uint8_t command,
		IOOperation* operation, bool offsetAsSectors);
		// Execute a command that involves a data transfer.
	void (*set_scan_semaphore)(void* controller, sem_id sem);
		// Pass the semaphore used for device rescan to the bus controller
	void (*set_bus_width)(void* controller, int width);
		// Set the data bus width to 1, 4 or 8 bit mode.
	void (*terminate_bus)(void* controller);
	void (*set_card_type)(void* controller, card_type type);
	status_t (*read_data)(void* controller, uint8_t command,
		uint32_t argument, void* buffer, size_t length);
		// Execute a command that reads a single small block of data (at most
		// 512 bytes) into a kernel buffer. Used for registers that are read
		// through the data lines, such as the eMMC EXT_CSD.
	status_t (*set_timing)(void* controller, int timing, uint32_t flags);
		// Configure the controller for an mmc_bus_timing, and the matching
		// clock (MMC_TIMING_* flags above). The device must already have been
		// switched to that timing. For HS200 this includes tuning. Returns an
		// error if the controller does not support the timing, or if tuning or
		// data transfers fail with it (the timing is then left as requested,
		// the caller decides what to do).
} mmc_bus_interface;


// Interface between mmc device driver (mmc_disk, sdio drivers, ...) and mmc_bus
// This interface is rather generic as it allows implementation of drivers for
// different type of cards, which will use different commands. The bus
// provides a generic interface for all of them, and is not specific to any
// type of card.
typedef struct mmc_device_interface {
	driver_module_info info;
	status_t (*execute_command)(device_node* node, void* cookie, uint16_t rca,
		uint8_t command, uint32_t argument, uint32_t* result);
		// Execute a command with no I/O phase
	status_t (*do_io)(device_node* controller, void* cookie, uint16_t rca,
		uint8_t command, IOOperation* operation, bool offsetAsSectors);
		// Execute a command that involves a data transfer.
	void (*set_bus_width)(device_node* controller, void* cookie, int width);
		// Set the data bus width to 1, 4 or 8 bit mode.
	status_t (*read_data)(device_node* controller, void* cookie, uint16_t rca,
		uint8_t command, uint32_t argument, void* buffer, size_t length);
		// Read a single small block of data (at most 512 bytes) into a
		// kernel buffer, see mmc_bus_interface::read_data.
	status_t (*set_timing)(device_node* controller, void* cookie, uint16_t rca,
		int timing, uint32_t flags);
		// See mmc_bus_interface::set_timing
} mmc_device_interface;


static inline bool
is_mmc_card_type(card_type type)
{
	return type == CARD_TYPE_MMC || type == CARD_TYPE_MMC_EXTENDED_CAPACITY;
}


// Device attribute paths for the MMC device
const char* const kMmcRcaAttribute = "mmc/rca";
const char* const kMmcTypeAttribute = "mmc/type";

// Device attributes of an SDIO function
const char* const kSdioFunctionAttribute = "sdio/function";
	// uint8, the function number, 1 to 7
const char* const kSdioVendorAttribute = "sdio/vendor";
	// uint16, the manufacturer code of the card (CISTPL_MANFID)
const char* const kSdioDeviceAttribute = "sdio/device";
	// uint16, the card id of the card (CISTPL_MANFID)
const char* const kSdioClassAttribute = "sdio/class";
	// uint8, the standard SDIO function interface code of the function


#endif /* _MMC_H */
