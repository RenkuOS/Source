/*
 * Copyright 2026, The RenkuOS authors.
 * Distributed under the terms of the MIT License.
 *
 * SPDX-License-Identifier: MIT
 *
 * CPU frequency scaling for the StarFive JH7110 (Milk-V Mars, VisionFive 2).
 *
 * The operating points come from the device tree ("operating-points-v2" of
 * the CPU nodes: 375, 500, 750 MHz at 0.8 V and 1.5 GHz at 1.04 V upstream).
 * The CPU clock is PLL0; the points used are those with a PLL0 preset of
 * fbdiv 125 in integer mode (as Linux's clk-starfive-jh7110-pll.c). While
 * PLL0 changes, the CPU runs from the 24 MHz oscillator, as the PLL0 notifier
 * of clk-starfive-jh7110-sys.c does.
 *
 * The CPU supply ("cpu-supply", DCDC2 of the AXP15060 PMIC on I2C) is set
 * once to the highest voltage of the table and never lowered, so every
 * point always runs at or above its voltage. Until the PMIC is reachable
 * (the I2C bus comes up after this module), the frequency is left as the
 * firmware set it.
 *
 * The points above MAX_MHZ (1.5 GHz upstream) are not used: the Mars hangs
 * at 75 C under load at 1.5 GHz. In their place come 1000, 1250 and 1375 MHz,
 * with the PLL0 presets of Linux (fbdiv/prediv 125/3, 625/12 and 1375/24,
 * integer mode), at the voltage of the replaced point. 1400 MHz as 175/3
 * hung the Mars; the presets were tried one by one and under load.
 *
 * A change runs the CPU from the oscillator, writes prediv before fbdiv (so
 * that PLL0 is never programmed far above the new rate in between), and
 * waits PLL_LOCK_WAIT with interrupts enabled before going back to PLL0.
 *
 * Low latency mode runs at the highest point; power saving mode moves
 * between the points with the load the scheduler reports. Both are limited
 * by the SoC temperature, read from the sensor register (enabled by the
 * jh7110_thermal driver; until it runs, the temperature is unknown): at or
 * above THERMAL_WARM_DECI the limit goes one point down every
 * THERMAL_STEP_INTERVAL, below
 * THERMAL_WARM_DECI - THERMAL_HYSTERESIS_DECI one point up, and from
 * THERMAL_HOT_DECI it is the lowest point at once. Until the temperature can
 * be read, the limit is the highest point up to 1000 MHz.
 */


#include <new>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <syscalls.h>
#include <cpufreq.h>
#include <device_manager.h>
#include <i2c.h>

extern "C" {
#include <libfdt.h>
}


#define JH7110_CPUFREQ_MODULE_NAME	CPUFREQ_MODULES_PREFIX "/jh7110_cpufreq/v1"

#define INFO(x...)	dprintf("jh7110_cpufreq: " x)
#define ERROR(x...)	dprintf("jh7110_cpufreq: " x)


extern void* gFDT;

// clock generator and PLL registers
#define SYSCRG_BASE				0x13020000
#define SYSSYSCON_BASE			0x13030000
#define CLK_CPU_ROOT			(0 * 4)		// mux: 0 = oscillator, 1 = PLL0
#define CLK_CPU_CORE			(1 * 4)		// divider
#define CLK_MUX_SHIFT			24
#define CLK_MUX_MASK			(0x3fu << CLK_MUX_SHIFT)
#define CLK_DIV_MASK			0x00ffffffu
#define PLL0_PD					0x18
#define PLL0_FBDIV				0x1c
#define PLL0_FRAC				0x20
#define PLL0_PREDIV				0x24
#define PLL0_DACPD				(1u << 24)
#define PLL0_DSMPD				(1u << 25)
#define PLL_FBDIV_MASK			0xfffu
#define PLL_PREDIV_MASK			0x3fu
#define PLL_POSTDIV1_MASK		(3u << 28)
#define PLL0_FBDIV_PRESET		125
// The module starts working START_DELAY after boot: working while the system
// boots hung the Mars about one boot in three, in the first read of the PMIC
// on its I2C bus.
#define START_DELAY				60000000

#define PLL_LOCK_WAIT			2000		// us; 1 ms was enough in tests
#define MAX_MHZ					1400
#define UNKNOWN_TEMP_MAX_MHZ	1000
#define OSC_MHZ					24

// AXP15060 PMIC
#define AXP15060_DCDC2_V_CTRL	0x14
#define AXP15060_DCDC2_MASK		0x7f

#define MAX_POINTS				8
#define SWITCH_INTERVAL			100000		// at most one change per 100 ms
// temperature sensor ("starfive,jh7110-temp"), see jh7110_thermal
#define SFCTEMP_RSTN			(1u << 0)
#define SFCTEMP_RUN				(1u << 2)
#define SFCTEMP_DOUT_SHIFT		16
#define SFCTEMP_DOUT_MASK		0xfffu
#define SFCTEMP_Y1000			237500
#define SFCTEMP_Z				4094
#define SFCTEMP_K1000			81100
#define THERMAL_STEP_INTERVAL	1000000		// one point per second
#define THERMAL_WARM_DECI		650			// 65 C: going down
#define THERMAL_HOT_DECI		700			// 70 C: the lowest point at once
#define THERMAL_HYSTERESIS_DECI	30			// up again below 62 C
#define POLL_INTERVAL			50000		// the scheduler hooks run with its locks
									// held: they only store the request, which
									// this thread polls


struct operating_point {
	uint32	mhz;
	uint32	microvolts;
	uint32	fbdiv;
	uint32	prediv;
};

// the extra points: PLL0 presets of Linux's clk-starfive-jh7110-pll.c
static const struct {
	uint32	fbdiv;
	uint32	prediv;
} kExtraPresets[] = {
	{ 125, 3 },		// 1000 MHz
	{ 625, 12 },	// 1250 MHz
	{ 1375, 24 },	// 1375 MHz
};


static device_manager_info* sDeviceManager;

static operating_point sPoints[MAX_POINTS];
static int32 sPointCount;
static uint32 sMaxMicrovolts;
static uint32 sPmicAddress;
static char sPmicBusName[64];		// device tree name of the PMIC's I2C node

static area_id sCrgArea = -1;
static area_id sSysconArea = -1;
static volatile uint32* sCrg;
static volatile uint32* sSyscon;
static spinlock sSwitchLock = B_SPINLOCK_INITIALIZER;

static thread_id sThread = -1;
static volatile bool sQuit;
static volatile bool sSupplyReady;
static volatile bool sPowerSaving;
static int32 sPerformance = kCPUPerformanceScaleMax;
	// 0 - kCPUPerformanceScaleMax, the scheduler's request
static int32 sCurrentPoint = -1;


// #pragma mark - device tree


static uint32
fdt_u32(const void* prop, int index)
{
	return B_BENDIAN_TO_HOST_INT32(((const uint32*)prop)[index]);
}


/*!	Reads the operating points of the first CPU and where its supply is.
	Keeps the points that have a PLL0 preset with fbdiv 125, up to MAX_MHZ;
	the points above are replaced by MAX_MHZ at the lowest of their voltages.
*/
static status_t
read_device_tree()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "starfive,jh7110") < 0) {
		return B_DEVICE_NOT_FOUND;
	}

	int cpu = fdt_node_offset_by_compatible(gFDT, -1, "sifive,u74-mc");
	if (cpu < 0)
		return B_DEVICE_NOT_FOUND;

	int length;
	const void* prop = fdt_getprop(gFDT, cpu, "operating-points-v2", &length);
	if (prop == NULL || length < 4)
		return B_DEVICE_NOT_FOUND;
	int table = fdt_node_offset_by_phandle(gFDT, fdt_u32(prop, 0));
	if (table < 0)
		return B_DEVICE_NOT_FOUND;

	uint32 maxMicrovoltsAbove = 0;
	int point;
	fdt_for_each_subnode(point, gFDT, table) {
		const void* hz = fdt_getprop(gFDT, point, "opp-hz", &length);
		if (hz == NULL || length < 8)
			continue;
		uint64 rate = ((uint64)fdt_u32(hz, 0) << 32) | fdt_u32(hz, 1);
		const void* volts = fdt_getprop(gFDT, point, "opp-microvolt", &length);
		if (volts == NULL || length < 4)
			continue;

		uint32 mhz = rate / 1000000;
		uint32 microvolts = fdt_u32(volts, 0);
		uint32 fbdiv = PLL0_FBDIV_PRESET;
		if (mhz > MAX_MHZ) {
			// replaced by MAX_MHZ at this point's voltage
			if (maxMicrovoltsAbove == 0 || microvolts < maxMicrovoltsAbove)
				maxMicrovoltsAbove = microvolts;
			continue;
		}
		uint32 osc = OSC_MHZ * fbdiv;
		if (mhz == 0 || osc % mhz != 0 || osc / mhz > PLL_PREDIV_MASK
			|| sPointCount == MAX_POINTS - B_COUNT_OF(kExtraPresets)) {
			INFO("%" B_PRIu32 " MHz has no PLL0 preset, not used\n", mhz);
			continue;
		}

		// sorted by frequency
		int32 index = sPointCount++;
		while (index > 0 && sPoints[index - 1].mhz > mhz) {
			sPoints[index] = sPoints[index - 1];
			index--;
		}
		sPoints[index].mhz = mhz;
		sPoints[index].microvolts = microvolts;
		sPoints[index].fbdiv = fbdiv;
		sPoints[index].prediv = osc / mhz;
		sMaxMicrovolts = max_c(sMaxMicrovolts, microvolts);
	}
	if (maxMicrovoltsAbove != 0) {
		// the extra points, above those of the table
		for (size_t i = 0; i < B_COUNT_OF(kExtraPresets); i++) {
			uint32 mhz = OSC_MHZ * kExtraPresets[i].fbdiv
				/ kExtraPresets[i].prediv;
			if (sPointCount == MAX_POINTS || mhz > MAX_MHZ
				|| (sPointCount > 0 && sPoints[sPointCount - 1].mhz >= mhz)) {
				continue;
			}
			operating_point& extra = sPoints[sPointCount++];
			extra.mhz = mhz;
			extra.microvolts = maxMicrovoltsAbove;
			extra.fbdiv = kExtraPresets[i].fbdiv;
			extra.prediv = kExtraPresets[i].prediv;
		}
		sMaxMicrovolts = max_c(sMaxMicrovolts, maxMicrovoltsAbove);
	}
	if (sPointCount == 0)
		return B_DEVICE_NOT_FOUND;

	// cpu-supply -> regulator node -> regulators -> PMIC -> I2C controller
	prop = fdt_getprop(gFDT, cpu, "cpu-supply", &length);
	if (prop == NULL || length < 4)
		return B_DEVICE_NOT_FOUND;
	int regulator = fdt_node_offset_by_phandle(gFDT, fdt_u32(prop, 0));
	int pmic = regulator >= 0 ? fdt_parent_offset(gFDT, regulator) : -1;
	if (pmic >= 0 && fdt_node_check_compatible(gFDT, pmic, "x-powers,axp15060"))
		pmic = fdt_parent_offset(gFDT, pmic);
	if (pmic < 0
		|| fdt_node_check_compatible(gFDT, pmic, "x-powers,axp15060") != 0) {
		ERROR("the CPU supply is not on an AXP15060\n");
		return B_DEVICE_NOT_FOUND;
	}
	const char* regulatorName = fdt_get_name(gFDT, regulator, NULL);
	if (regulatorName == NULL || strcmp(regulatorName, "dcdc2") != 0) {
		ERROR("the CPU supply is not DCDC2\n");
		return B_DEVICE_NOT_FOUND;
	}
	prop = fdt_getprop(gFDT, pmic, "reg", &length);
	if (prop == NULL || length < 4)
		return B_DEVICE_NOT_FOUND;
	sPmicAddress = fdt_u32(prop, 0);

	int bus = fdt_parent_offset(gFDT, pmic);
	const char* busName = bus >= 0 ? fdt_get_name(gFDT, bus, NULL) : NULL;
	if (busName == NULL)
		return B_DEVICE_NOT_FOUND;
	strlcpy(sPmicBusName, busName, sizeof(sPmicBusName));
	return B_OK;
}


// #pragma mark - PMIC


static uint32
dcdc2_microvolts(uint8 code)
{
	code &= AXP15060_DCDC2_MASK;
	if (code <= 70)
		return 500000 + 10000 * code;
	return 1220000 + 20000 * (code - 71);
}


static int32
dcdc2_code(uint32 microvolts)
{
	// the lowest code at or above the voltage
	for (int32 code = 0; code <= 87; code++) {
		if (dcdc2_microvolts(code) >= microvolts)
			return code;
	}
	return -1;
}


/*!	The I2C bus node whose controller is the device tree node named
	sPmicBusName. The node is acquired.
*/
static device_node*
find_pmic_bus()
{
	device_attr attrs[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "i2c" }},
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "I2C bus" }},
		{}
	};
	device_node* root = sDeviceManager->get_root_node();
	device_node* bus = NULL;
	while (sDeviceManager->find_child_node(root, attrs, &bus) == B_OK) {
		// walk up to the device tree node of the controller
		device_node* node = sDeviceManager->get_parent_node(bus);
		while (node != NULL) {
			const char* name;
			if (sDeviceManager->get_attr_string(node, "fdt/name", &name,
					false) == B_OK) {
				bool match = strcmp(name, sPmicBusName) == 0;
				sDeviceManager->put_node(node);
				if (match) {
					sDeviceManager->put_node(root);
					return bus;
				}
				break;
			}
			device_node* parent = sDeviceManager->get_parent_node(node);
			sDeviceManager->put_node(node);
			node = parent;
		}
	}
	sDeviceManager->put_node(root);
	return NULL;
}


static int sBusRawFD = -1;


/*!	Keeps the driver of the PMIC's bus initialized: get_driver() does not, and
	right after the bus appears its driver may still be initializing, or be
	uninitialized again as unused at the end of the device scan. Opening the
	raw bus device waits for the initialization and holds it while open.
*/
static status_t
hold_bus(device_node* busNode)
{
	if (sBusRawFD >= 0)
		return B_OK;
	uint8 pathID;
	if (sDeviceManager->get_attr_uint8(busNode, I2C_BUS_PATH_ID_ITEM, &pathID,
			false) != B_OK) {
		return B_BUSY;
	}
	char path[64];
	snprintf(path, sizeof(path), "/dev/bus/i2c/%u/bus_raw", pathID);
	sBusRawFD = _kern_open(-1, path, O_RDWR, 0);
	if (sBusRawFD < 0)
		return B_BUSY;
	return B_OK;
}


static status_t
pmic_transfer(device_node* busNode, i2c_op op, uint8 reg, uint8* value)
{
	i2c_bus_interface* bus;
	i2c_bus cookie;
	if (sDeviceManager->get_driver(busNode, (driver_module_info**)&bus,
			(void**)&cookie) != B_OK) {
		return B_NO_INIT;
	}
	status_t status = bus->acquire_bus(cookie);
	if (status != B_OK)
		return status;
	status = bus->exec_command(cookie, op, sPmicAddress, &reg, 1, value, 1);
	bus->release_bus(cookie);
	return status;
}


/*!	Raises the CPU supply to the highest voltage of the table, if it is
	lower. Returns B_OK once the supply is known to be high enough.
*/
static status_t
prepare_supply()
{
	device_node* busNode = find_pmic_bus();
	if (busNode == NULL)
		return B_BUSY;
	if (hold_bus(busNode) != B_OK) {
		sDeviceManager->put_node(busNode);
		return B_BUSY;
	}

	status_t status;
	uint8 code;
	if ((status = pmic_transfer(busNode, I2C_OP_READ_STOP,
			AXP15060_DCDC2_V_CTRL, &code)) != B_OK) {
		sDeviceManager->put_node(busNode);
		return status;
	}

	uint32 current = dcdc2_microvolts(code);
	int32 wanted = dcdc2_code(sMaxMicrovolts);
	if (wanted < 0) {
		sDeviceManager->put_node(busNode);
		return B_BAD_VALUE;
	}

	if (current < sMaxMicrovolts) {
		uint8 newCode = (code & ~AXP15060_DCDC2_MASK) | wanted;
		status = pmic_transfer(busNode, I2C_OP_WRITE_STOP,
			AXP15060_DCDC2_V_CTRL, &newCode);
		snooze(20000);
		uint8 check = 0;
		if (status == B_OK) {
			status = pmic_transfer(busNode, I2C_OP_READ_STOP,
				AXP15060_DCDC2_V_CTRL, &check);
		}
		if (status != B_OK
			|| (check & AXP15060_DCDC2_MASK) != (uint8)wanted) {
			ERROR("could not set the CPU supply (%s, reads %#x)\n",
				strerror(status), check);
			sDeviceManager->put_node(busNode);
			return B_ERROR;
		}
		INFO("CPU supply %" B_PRIu32 " -> %" B_PRIu32 " mV\n",
			current / 1000, dcdc2_microvolts(check) / 1000);
	} else {
		INFO("CPU supply %" B_PRIu32 " mV\n", current / 1000);
	}

	sDeviceManager->put_node(busNode);
	return B_OK;
}


// #pragma mark - PLL0


static inline uint32
crg(uint32 offset)
{
	return sCrg[offset / 4];
}


static inline uint32
syscon(uint32 offset)
{
	return sSyscon[offset / 4];
}


static bool
known_fbdiv(uint32 fbdiv)
{
	if (fbdiv == PLL0_FBDIV_PRESET)
		return true;
	for (size_t i = 0; i < B_COUNT_OF(kExtraPresets); i++) {
		if (fbdiv == kExtraPresets[i].fbdiv)
			return true;
	}
	return false;
}


static bool
known_clock_state()
{
	uint32 prediv = syscon(PLL0_PREDIV) & PLL_PREDIV_MASK;
	return ((crg(CLK_CPU_ROOT) & CLK_MUX_MASK) >> CLK_MUX_SHIFT) == 1
		&& (crg(CLK_CPU_CORE) & CLK_DIV_MASK) == 1
		&& (syscon(PLL0_PD) & (PLL0_DACPD | PLL0_DSMPD))
			== (PLL0_DACPD | PLL0_DSMPD)
		&& known_fbdiv(syscon(PLL0_FBDIV) & PLL_FBDIV_MASK)
		&& (syscon(PLL0_FRAC) & PLL_POSTDIV1_MASK) == 0
		&& prediv >= 2 && prediv <= PLL_PREDIV_MASK;
}


static status_t
set_point(int32 index)
{
	uint32 fbdiv = sPoints[index].fbdiv;
	uint32 prediv = sPoints[index].prediv;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sSwitchLock);

	if (!known_clock_state()) {
		release_spinlock(&sSwitchLock);
		restore_interrupts(state);
		return B_NOT_ALLOWED;
	}

	// CPU on the oscillator while PLL0 changes; prediv first, so that PLL0
	// is not programmed far above the new rate in between
	sCrg[CLK_CPU_ROOT / 4] = (crg(CLK_CPU_ROOT) & ~CLK_MUX_MASK);
	spin(10);
	sSyscon[PLL0_PREDIV / 4] = (syscon(PLL0_PREDIV) & ~PLL_PREDIV_MASK)
		| prediv;
	sSyscon[PLL0_FBDIV / 4] = (syscon(PLL0_FBDIV) & ~PLL_FBDIV_MASK) | fbdiv;

	release_spinlock(&sSwitchLock);
	restore_interrupts(state);

	// the lock time, with interrupts enabled (the CPU runs at 24 MHz)
	snooze(PLL_LOCK_WAIT);

	state = disable_interrupts();
	acquire_spinlock(&sSwitchLock);
	sCrg[CLK_CPU_ROOT / 4] = (crg(CLK_CPU_ROOT) & ~CLK_MUX_MASK)
		| (1u << CLK_MUX_SHIFT);
	spin(10);
	release_spinlock(&sSwitchLock);
	restore_interrupts(state);

	sCurrentPoint = index;
	return B_OK;
}


// #pragma mark - policy


static area_id sSensorArea = -1;
static volatile uint32* sSensor;
static int32 sThermalLimit = -1;
	// highest point the temperature allows; -1 before the first reading
static bigtime_t sLastThermalCheck;


/*!	Maps the register of the temperature sensor of the device tree. Only
	read here: the jh7110_thermal driver enables the sensor.
*/
static void
map_sensor()
{
	int node = fdt_node_offset_by_compatible(gFDT, -1, "starfive,jh7110-temp");
	if (node < 0)
		return;
	int length;
	const void* reg = fdt_getprop(gFDT, node, "reg", &length);
	int parent = fdt_parent_offset(gFDT, node);
	int cellsLength;
	const void* cells = parent >= 0
		? fdt_getprop(gFDT, parent, "#address-cells", &cellsLength) : NULL;
	int addressCells = cells != NULL && cellsLength == 4
		? (int)fdt_u32(cells, 0) : -1;
	if (reg == NULL || addressCells < 1 || addressCells > 2
		|| length < addressCells * 4) {
		return;
	}
	phys_addr_t address = fdt_u32(reg, 0);
	if (addressCells == 2)
		address = (address << 32) | fdt_u32(reg, 1);
	sSensorArea = map_physical_memory("jh7110 cpufreq sensor", address,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA,
		(void**)&sSensor);
	if (sSensorArea < 0)
		sSensor = NULL;
}


/*!	Temperature in tenths of a degree Celsius, or -1 while the sensor does
	not run.
*/
static int32
read_temperature()
{
	if (sSensor == NULL)
		return -1;
	uint32 value = *sSensor;
	if ((value & (SFCTEMP_RSTN | SFCTEMP_RUN)) != (SFCTEMP_RSTN | SFCTEMP_RUN))
		return -1;
	uint32 dout = (value >> SFCTEMP_DOUT_SHIFT) & SFCTEMP_DOUT_MASK;
	int64 milli = (int64)dout * SFCTEMP_Y1000 / SFCTEMP_Z - SFCTEMP_K1000;
	return milli < 0 ? 0 : (int32)(milli / 100);
}


static int32
unknown_temperature_limit()
{
	int32 limit = 0;
	for (int32 i = 0; i < sPointCount; i++) {
		if (sPoints[i].mhz <= UNKNOWN_TEMP_MAX_MHZ)
			limit = i;
	}
	return limit;
}


static void
update_thermal_limit()
{
	bigtime_t now = system_time();
	if (sThermalLimit >= 0 && now - sLastThermalCheck < THERMAL_STEP_INTERVAL)
		return;
	sLastThermalCheck = now;

	int32 top = sPointCount - 1;
	int32 temperature = read_temperature();
	int32 limit = sThermalLimit;
	if (temperature < 0)
		limit = unknown_temperature_limit();
	else if (limit < 0)
		limit = temperature < THERMAL_WARM_DECI ? top : 0;
	else if (temperature >= THERMAL_HOT_DECI)
		limit = 0;
	else if (temperature >= THERMAL_WARM_DECI)
		limit = max_c(limit - 1, 0);
	else if (temperature < THERMAL_WARM_DECI - THERMAL_HYSTERESIS_DECI)
		limit = min_c(limit + 1, top);

	if (limit != sThermalLimit && temperature >= 0) {
		INFO("%" B_PRId32 ".%" B_PRId32 " C: up to %" B_PRIu32 " MHz\n",
			temperature / 10, temperature % 10, sPoints[limit].mhz);
	}
	sThermalLimit = limit;
}


static int32
wanted_point()
{
	int32 point = sPointCount - 1;
	if (sPowerSaving) {
		int32 performance = atomic_get(&sPerformance);
		point = performance * (sPointCount - 1) / kCPUPerformanceScaleMax;
	}
	return min_c(point, sThermalLimit);
}


static status_t
cpufreq_thread(void*)
{
	while (!sQuit && system_time() < START_DELAY)
		snooze(1000000);
	if (sQuit)
		return B_OK;

	// wait for the PMIC's I2C bus: up to two minutes after this module
	for (int32 i = 0; i < 120 && !sQuit; i++) {
		status_t status = prepare_supply();
		if (status == B_OK) {
			sSupplyReady = true;
			break;
		}
		if (status != B_BUSY && status != B_NO_INIT) {
			ERROR("CPU supply not usable, frequency left as it is\n");
			return status;
		}
		snooze(1000000);
	}
	if (!sSupplyReady) {
		ERROR("PMIC not reachable, frequency left as it is\n");
		return B_TIMED_OUT;
	}

	bigtime_t lastSwitch = 0;
	while (!sQuit) {
		update_thermal_limit();
		int32 point = wanted_point();
		if (point != sCurrentPoint) {
			bigtime_t now = system_time();
			if (now - lastSwitch < SWITCH_INTERVAL) {
				snooze(SWITCH_INTERVAL - (now - lastSwitch));
				continue;
			}
			uint32 from = sCurrentPoint >= 0 ? sPoints[sCurrentPoint].mhz : 0;
			status_t status = set_point(point);
			if (status != B_OK) {
				ERROR("unexpected clock state, stopping\n");
				return status;
			}
			lastSwitch = system_time();
			if (from == 0) {
				INFO("CPU at %" B_PRIu32 " MHz\n", sPoints[point].mhz);
			}
		}
		snooze(POLL_INTERVAL);
	}
	return B_OK;
}


// #pragma mark - module


static void
jh7110_set_scheduler_mode(scheduler_mode mode)
{
	sPowerSaving = mode == SCHEDULER_MODE_POWER_SAVING;
}


static status_t
jh7110_increase_performance(int delta)
{
	int32 old = atomic_get(&sPerformance);
	int32 value = min_c(old + delta, kCPUPerformanceScaleMax);
	atomic_set(&sPerformance, value);
	return B_OK;
}


static status_t
jh7110_decrease_performance(int delta)
{
	int32 old = atomic_get(&sPerformance);
	int32 value = max_c(old - delta, 0);
	atomic_set(&sPerformance, value);
	return B_OK;
}


static void
uninit()
{
	if (sThread >= 0) {
		sQuit = true;
		status_t result;
		wait_for_thread(sThread, &result);
		sThread = -1;
	}
	if (sBusRawFD >= 0)
		_kern_close(sBusRawFD);
	sBusRawFD = -1;
	if (sSensorArea >= 0)
		delete_area(sSensorArea);
	sSensorArea = -1;
	sSensor = NULL;
	if (sCrgArea >= 0)
		delete_area(sCrgArea);
	if (sSysconArea >= 0)
		delete_area(sSysconArea);
	sCrgArea = sSysconArea = -1;
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
}


static status_t
init()
{
	if (get_module(B_DEVICE_MANAGER_MODULE_NAME,
			(module_info**)&sDeviceManager) != B_OK) {
		return B_ERROR;
	}

	status_t status = read_device_tree();
	if (status != B_OK) {
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return status;
	}

	sCrgArea = map_physical_memory("jh7110 syscrg", SYSCRG_BASE, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&sCrg);
	sSysconArea = map_physical_memory("jh7110 sys syscon", SYSSYSCON_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sSyscon);
	if (sCrgArea < 0 || sSysconArea < 0) {
		uninit();
		return B_ERROR;
	}
	if (!known_clock_state()) {
		ERROR("CPU clock not in a known state, not used\n");
		uninit();
		return B_NOT_SUPPORTED;
	}

	map_sensor();

	char points[128] = "";
	for (int32 i = 0; i < sPointCount; i++) {
		char point[32];
		snprintf(point, sizeof(point), "%s%" B_PRIu32 " MHz/%" B_PRIu32 " mV",
			i > 0 ? ", " : "", sPoints[i].mhz,
			sPoints[i].microvolts / 1000);
		strlcat(points, point, sizeof(points));
	}
	INFO("points %s; supply on the AXP15060 at %#" B_PRIx32 " of %s\n",
		points, sPmicAddress, sPmicBusName);

	sThread = spawn_kernel_thread(cpufreq_thread, "jh7110 cpufreq",
		B_LOW_PRIORITY, NULL);
	if (sThread < 0) {
		uninit();
		return B_NO_MEMORY;
	}
	resume_thread(sThread);
	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return init();
		case B_MODULE_UNINIT:
			uninit();
			return B_OK;
	}
	return B_ERROR;
}


static cpufreq_module_info sJh7110CpufreqModule = {
	{
		JH7110_CPUFREQ_MODULE_NAME,
		0,
		std_ops,
	},

	1.0f,

	jh7110_set_scheduler_mode,
	jh7110_increase_performance,
	jh7110_decrease_performance,
};


module_info* modules[] = {
	(module_info*)&sJh7110CpufreqModule,
	NULL
};
