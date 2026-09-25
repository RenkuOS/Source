/*
 * Copyright 2005-2013, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jan-Rixt Van Hoye
 *		Salvatore Benedetto <salvatore.benedetto@gmail.com>
 *		Michael Lotz <mmlr@mlotz.ch>
 *		Siarzhuk Zharski <imker@gmx.li>
 */


#include <stdio.h>

#include <module.h>
#include <bus/PCI.h>
#include <USB3.h>
#include <KernelExport.h>
#include <util/AutoLock.h>

#include "ohci.h"


#define CALLED(x...)	TRACE_MODULE("CALLED %s\n", __PRETTY_FUNCTION__)

#define USB_MODULE_NAME "ohci"

device_manager_info* gDeviceManager;
static usb_for_controller_interface* gUSB;


#define OHCI_PCI_DEVICE_MODULE_NAME "busses/usb/ohci/pci/driver_v1"
#define OHCI_PCI_USB_BUS_MODULE_NAME "busses/usb/ohci/device_v1"


typedef struct {
	OHCI* ohci;
	pci_device_module_info* pci;
	pci_device* device;

	pci_info pciinfo;

	device_node* node;
	device_node* driver_node;
} ohci_pci_sim_info;


//	#pragma mark -


static status_t
init_bus(device_node* node, void** bus_cookie)
{
	CALLED();

	driver_module_info* driver;
	ohci_pci_sim_info* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	gDeviceManager->get_driver(parent, &driver, (void**)&bus);
	gDeviceManager->put_node(parent);

	Stack *stack;
	if (gUSB->get_stack((void**)&stack) != B_OK)
		return B_ERROR;

	OHCI *ohci = new(std::nothrow) OHCI(&bus->pciinfo, bus->pci, bus->device, stack, node);
	if (ohci == NULL) {
		return B_NO_MEMORY;
	}

	if (ohci->InitCheck() < B_OK) {
		TRACE_MODULE_ERROR("bus failed init check\n");
		delete ohci;
		return B_ERROR;
	}

	if (ohci->Start() != B_OK) {
		delete ohci;
		return B_ERROR;
	}

	*bus_cookie = ohci;

	return B_OK;
}


static void
uninit_bus(void* bus_cookie)
{
	CALLED();
	OHCI* ohci = (OHCI*)bus_cookie;
	delete ohci;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	ohci_pci_sim_info* bus = (ohci_pci_sim_info*)cookie;
	device_node* node = bus->driver_node;

	char prettyName[25];
	sprintf(prettyName, "OHCI Controller %" B_PRIu16, 0);

	device_attr attrs[] = {
		// properties of this controller for the usb bus manager
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = prettyName }},
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = USB_FOR_CONTROLLER_MODULE_NAME }},

		// private data to identify the device
		{ NULL }
	};

	return gDeviceManager->register_node(node, OHCI_PCI_USB_BUS_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
init_device(device_node* node, void** device_cookie)
{
	CALLED();
	ohci_pci_sim_info* bus = (ohci_pci_sim_info*)calloc(1,
		sizeof(ohci_pci_sim_info));
	if (bus == NULL)
		return B_NO_MEMORY;

	pci_device_module_info* pci;
	pci_device* device;
	{
		device_node* pciParent = gDeviceManager->get_parent_node(node);
		gDeviceManager->get_driver(pciParent, (driver_module_info**)&pci,
			(void**)&device);
		gDeviceManager->put_node(pciParent);
	}

	bus->pci = pci;
	bus->device = device;
	bus->driver_node = node;

	pci_info *pciInfo = &bus->pciinfo;
	pci->get_pci_info(device, pciInfo);

	*device_cookie = bus;
	return B_OK;
}


static void
uninit_device(void* device_cookie)
{
	CALLED();
	ohci_pci_sim_info* bus = (ohci_pci_sim_info*)device_cookie;
	free(bus);
}


static status_t
register_device(device_node* parent)
{
	CALLED();
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "OHCI PCI"}},
		{}
	};

	return gDeviceManager->register_node(parent,
		OHCI_PCI_DEVICE_MODULE_NAME, attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	CALLED();
	const char* bus;
	uint16 type, subType, api;

	// make sure parent is a OHCI PCI device node
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
		< B_OK) {
		return -1;
	}

	if (strcmp(bus, "pci") != 0)
		return 0.0f;

	if (gDeviceManager->get_attr_uint16(parent, B_DEVICE_SUB_TYPE, &subType,
			false) < B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_TYPE, &type,
			false) < B_OK
		|| gDeviceManager->get_attr_uint16(parent, B_DEVICE_INTERFACE, &api,
			false) < B_OK) {
		TRACE_MODULE("Could not find type/subtype/interface attributes\n");
		return -1;
	}

	if (type == PCI_serial_bus && subType == PCI_usb && api == PCI_usb_ohci) {
		pci_device_module_info* pci;
		pci_device* device;
		gDeviceManager->get_driver(parent, (driver_module_info**)&pci,
			(void**)&device);
		TRACE_MODULE("OHCI Device found!\n");

		return 0.8f;
	}

	return 0.0f;
}


module_dependency module_dependencies[] = {
	{ USB_FOR_CONTROLLER_MODULE_NAME, (module_info**)&gUSB },
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


static usb_bus_interface gOHCIPCIDeviceModule = {
	{
		{
			OHCI_PCI_USB_BUS_MODULE_NAME,
			0,
			NULL
		},
		NULL,  // supports device
		NULL,  // register device
		init_bus,
		uninit_bus,
		NULL,  // register child devices
		NULL,  // rescan
		NULL,  // device removed
	},
};

// Root device that binds to the PCI bus. It will register an usb_bus_interface
// node for each device.
static driver_module_info sOHCIDevice = {
	{
		OHCI_PCI_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL, // rescan
	NULL, // device removed
};

module_info* modules[] = {
	(module_info* )&sOHCIDevice,
	(module_info* )&gOHCIPCIDeviceModule,
	NULL
};


//
// #pragma mark -
//


OHCI::OHCI(pci_info *info, pci_device_module_info* pci, pci_device* device, Stack *stack,
	device_node* node)
	:	BusManager(stack, node),
		fPCIInfo(info),
		fPci(pci),
		fDevice(device),
		fStack(stack),
		fOperationalRegisters(NULL),
		fRegisterArea(-1),
		fHccaArea(-1),
		fHcca(NULL),
		fInterruptEndpoints(NULL),
		fDummyControl(NULL),
		fDummyBulk(NULL),
		fDummyIsochronous(NULL),
		fFirstTransfer(NULL),
		fLastTransfer(NULL),
		fFinishTransfersSem(-1),
		fFinishThread(-1),
		fStopFinishThread(false),
		fProcessingPipe(NULL),
		fFrameBandwidth(NULL),
		fRootHub(NULL),
		fRootHubAddress(0),
		fPortCount(0),
		fIRQ(0),
		fUseMSI(false)
{
	if (!fInitOK) {
		TRACE_ERROR("bus manager failed to init\n");
		return;
	}

	TRACE("constructing new OHCI host controller driver\n");
	fInitOK = false;

	mutex_init(&fEndpointLock, "ohci endpoint lock");

	// enable busmaster and memory mapped access
	uint16 command = fPci->read_pci_config(fDevice, PCI_command, 2);
	command &= ~PCI_command_io;
	command |= PCI_command_master | PCI_command_memory;

	fPci->write_pci_config(fDevice, PCI_command, 2, command);

	// map the registers
	uint32 offset = fPci->read_pci_config(fDevice, PCI_base_registers, 4);
	offset &= PCI_address_memory_32_mask;
	TRACE_ALWAYS("iospace offset: 0x%" B_PRIx32 "\n", offset);
	fRegisterArea = map_physical_memory("OHCI memory mapped registers",
		offset,	B_PAGE_SIZE, B_ANY_KERNEL_BLOCK_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void **)&fOperationalRegisters);
	if (fRegisterArea < B_OK) {
		TRACE_ERROR("failed to map register memory\n");
		return;
	}

	TRACE("mapped operational registers: %p\n", fOperationalRegisters);

	// Check the revision of the controller, which should be 10h
	uint32 revision = _ReadReg(OHCI_REVISION) & 0xff;
	TRACE("version %" B_PRId32 ".%" B_PRId32 "%s\n",
		OHCI_REVISION_HIGH(revision), OHCI_REVISION_LOW(revision),
		OHCI_REVISION_LEGACY(revision) ? ", legacy support" : "");

	if (OHCI_REVISION_HIGH(revision) != 1 || OHCI_REVISION_LOW(revision) != 0) {
		TRACE_ERROR("unsupported OHCI revision\n");
		return;
	}

	phys_addr_t hccaPhysicalAddress;
	fHccaArea = fStack->AllocateArea((void **)&fHcca, &hccaPhysicalAddress,
		sizeof(ohci_hcca), "USB OHCI Host Controller Communication Area");

	if (fHccaArea < B_OK) {
		TRACE_ERROR("unable to create the HCCA block area\n");
		return;
	}

	memset(fHcca, 0, sizeof(ohci_hcca));

	// Set Up Host controller
	// Dummy endpoints
	fDummyControl = _AllocateEndpoint();
	if (!fDummyControl)
		return;

	fDummyBulk = _AllocateEndpoint();
	if (!fDummyBulk) {
		_FreeEndpoint(fDummyControl);
		return;
	}

	fDummyIsochronous = _AllocateEndpoint();
	if (!fDummyIsochronous) {
		_FreeEndpoint(fDummyControl);
		_FreeEndpoint(fDummyBulk);
		return;
	}

	// Static endpoints that get linked in the HCCA
	fInterruptEndpoints = new(std::nothrow)
		ohci_endpoint_descriptor *[OHCI_STATIC_ENDPOINT_COUNT];
	if (!fInterruptEndpoints) {
		TRACE_ERROR("failed to allocate memory for interrupt endpoints\n");
		_FreeEndpoint(fDummyControl);
		_FreeEndpoint(fDummyBulk);
		_FreeEndpoint(fDummyIsochronous);
		return;
	}

	for (int32 i = 0; i < OHCI_STATIC_ENDPOINT_COUNT; i++) {
		fInterruptEndpoints[i] = _AllocateEndpoint();
		if (!fInterruptEndpoints[i]) {
			TRACE_ERROR("failed to allocate interrupt endpoint %" B_PRId32 "\n",
				i);
			while (--i >= 0)
				_FreeEndpoint(fInterruptEndpoints[i]);
			_FreeEndpoint(fDummyBulk);
			_FreeEndpoint(fDummyControl);
			_FreeEndpoint(fDummyIsochronous);
			return;
		}
	}

	// build flat tree so that at each of the static interrupt endpoints
	// fInterruptEndpoints[i] == interrupt endpoint for interval 2^i
	uint32 interval = OHCI_BIGGEST_INTERVAL;
	uint32 intervalIndex = OHCI_STATIC_ENDPOINT_COUNT - 1;
	while (interval > 1) {
		uint32 insertIndex = interval / 2;
		while (insertIndex < OHCI_BIGGEST_INTERVAL) {
			fHcca->interrupt_table[insertIndex]
				= fInterruptEndpoints[intervalIndex]->physical_address;
			insertIndex += interval;
		}

		intervalIndex--;
		interval /= 2;
	}

	// setup the empty slot in the list and linking of all -> first
	fHcca->interrupt_table[0] = fInterruptEndpoints[0]->physical_address;
	for (int32 i = 1; i < OHCI_STATIC_ENDPOINT_COUNT; i++) {
		fInterruptEndpoints[i]->next_physical_endpoint
			= fInterruptEndpoints[0]->physical_address;
		fInterruptEndpoints[i]->next_logical_endpoint
			= fInterruptEndpoints[0];
	}

	// Now link the first endpoint to the isochronous endpoint
	fInterruptEndpoints[0]->next_physical_endpoint
		= fDummyIsochronous->physical_address;

	// When the handover from SMM takes place, all interrupts are routed to the
	// OS. As we don't yet have an interrupt handler installed at this point,
	// this may cause interrupt storms if the firmware does not disable the
	// interrupts during handover. Therefore we disable interrupts before
	// requesting ownership. We have to keep the ownership change interrupt
	// enabled though, as otherwise the SMM will not be notified of the
	// ownership change request we trigger below.
	_WriteReg(OHCI_INTERRUPT_DISABLE, OHCI_ALL_INTERRUPTS &
		~OHCI_OWNERSHIP_CHANGE) ;

	// Determine in what context we are running (Kindly copied from FreeBSD)
	uint32 control = _ReadReg(OHCI_CONTROL);
	if (control & OHCI_INTERRUPT_ROUTING) {
		TRACE_ALWAYS("smm is in control of the host controller\n");
		uint32 status = _ReadReg(OHCI_COMMAND_STATUS);
		_WriteReg(OHCI_COMMAND_STATUS, status | OHCI_OWNERSHIP_CHANGE_REQUEST);
		for (uint32 i = 0; i < 100 && (control & OHCI_INTERRUPT_ROUTING); i++) {
			snooze(1000);
			control = _ReadReg(OHCI_CONTROL);
		}

		if ((control & OHCI_INTERRUPT_ROUTING) != 0) {
			TRACE_ERROR("smm does not respond.\n");

			// TODO: Enable this reset as soon as the non-specified
			// reset a few lines later is replaced by a better solution.
			//_WriteReg(OHCI_CONTROL, OHCI_HC_FUNCTIONAL_STATE_RESET);
			//snooze(USB_DELAY_BUS_RESET);
		} else
			TRACE_ALWAYS("ownership change successful\n");
	} else {
		TRACE("cold started\n");
		snooze(USB_DELAY_BUS_RESET);
	}

	// TODO: This reset delays system boot time. It should not be necessary
	// according to the OHCI spec, but without it some controllers don't start.
	_WriteReg(OHCI_CONTROL, OHCI_HC_FUNCTIONAL_STATE_RESET);
	snooze(USB_DELAY_BUS_RESET);

	// We now own the host controller and the bus has been reset
	uint32 frameInterval = _ReadReg(OHCI_FRAME_INTERVAL);
	uint32 intervalValue = OHCI_GET_INTERVAL_VALUE(frameInterval);

	_WriteReg(OHCI_COMMAND_STATUS, OHCI_HOST_CONTROLLER_RESET);
	// Nominal time for a reset is 10 us
	uint32 reset = 0;
	for (uint32 i = 0; i < 10; i++) {
		spin(10);
		reset = _ReadReg(OHCI_COMMAND_STATUS) & OHCI_HOST_CONTROLLER_RESET;
		if (reset == 0)
			break;
	}

	if (reset) {
		TRACE_ERROR("error resetting the host controller (timeout)\n");
		return;
	}

	// The controller is now in SUSPEND state, we have 2ms to go OPERATIONAL.

	// Set up host controller register
	_WriteReg(OHCI_HCCA, (uint32)hccaPhysicalAddress);
	_WriteReg(OHCI_CONTROL_HEAD_ED, (uint32)fDummyControl->physical_address);
	_WriteReg(OHCI_BULK_HEAD_ED, (uint32)fDummyBulk->physical_address);
	// Switch on desired functional features
	control = _ReadReg(OHCI_CONTROL);
	control &= ~(OHCI_CONTROL_BULK_SERVICE_RATIO_MASK | OHCI_ENABLE_LIST
		| OHCI_HC_FUNCTIONAL_STATE_MASK | OHCI_INTERRUPT_ROUTING);
	control |= OHCI_ENABLE_LIST | OHCI_CONTROL_BULK_RATIO_1_4
		| OHCI_HC_FUNCTIONAL_STATE_OPERATIONAL;
	// And finally start the controller
	_WriteReg(OHCI_CONTROL, control);

	// The controller is now OPERATIONAL.
	frameInterval = (_ReadReg(OHCI_FRAME_INTERVAL) & OHCI_FRAME_INTERVAL_TOGGLE)
		^ OHCI_FRAME_INTERVAL_TOGGLE;
	frameInterval |= OHCI_FSMPS(intervalValue) | intervalValue;
	_WriteReg(OHCI_FRAME_INTERVAL, frameInterval);
	// 90% periodic
	uint32 periodic = OHCI_PERIODIC(intervalValue);
	_WriteReg(OHCI_PERIODIC_START, periodic);

	// Fiddle the No Over Current Protection bit to avoid chip bug
	uint32 desca = _ReadReg(OHCI_RH_DESCRIPTOR_A);
	_WriteReg(OHCI_RH_DESCRIPTOR_A, desca | OHCI_RH_NO_OVER_CURRENT_PROTECTION);
	_WriteReg(OHCI_RH_STATUS, OHCI_RH_LOCAL_POWER_STATUS_CHANGE);
	snooze(OHCI_ENABLE_POWER_DELAY);
	_WriteReg(OHCI_RH_DESCRIPTOR_A, desca);

	// The AMD756 requires a delay before re-reading the register,
	// otherwise it will occasionally report 0 ports.
	uint32 numberOfPorts = 0;
	for (uint32 i = 0; i < 10 && numberOfPorts == 0; i++) {
		snooze(OHCI_READ_DESC_DELAY);
		uint32 descriptor = _ReadReg(OHCI_RH_DESCRIPTOR_A);
		numberOfPorts = OHCI_RH_GET_PORT_COUNT(descriptor);
	}
	if (numberOfPorts > OHCI_MAX_PORT_COUNT)
		numberOfPorts = OHCI_MAX_PORT_COUNT;
	fPortCount = numberOfPorts;
	TRACE("port count is %d\n", fPortCount);

	// Create the array that will keep bandwidth information
	fFrameBandwidth = new(std::nothrow) uint16[NUMBER_OF_FRAMES];

	for (int32 i = 0; i < NUMBER_OF_FRAMES; i++)
		fFrameBandwidth[i] = MAX_AVAILABLE_BANDWIDTH;

	// Create semaphore the finisher thread will wait for
	fFinishTransfersSem = create_sem(0, "OHCI Finish Transfers");
	if (fFinishTransfersSem < B_OK) {
		TRACE_ERROR("failed to create semaphore\n");
		return;
	}

	// Create the finisher service thread
	fFinishThread = spawn_kernel_thread(_FinishThread, "ohci finish thread",
		B_URGENT_DISPLAY_PRIORITY, (void *)this);
	resume_thread(fFinishThread);

	// Find the right interrupt vector, using MSIs if available.
	fIRQ = fPCIInfo->u.h0.interrupt_line;
	if (fIRQ == 0xFF)
		fIRQ = 0;

	if (fPci->get_msi_count(fDevice) >= 1) {
		uint32 msiVector = 0;
		if (fPci->configure_msi(fDevice, 1, &msiVector) == B_OK
			&& fPci->enable_msi(fDevice) == B_OK) {
			TRACE_ALWAYS("using message signaled interrupts\n");
			fIRQ = msiVector;
			fUseMSI = true;
		}
	}

	if (fIRQ == 0) {
		TRACE_MODULE_ERROR("device PCI:%d:%d:%d was assigned an invalid IRQ\n",
			fPCIInfo->bus, fPCIInfo->device, fPCIInfo->function);
		return;
	}

	// Install the interrupt handler
	TRACE("installing interrupt handler\n");
	install_io_interrupt_handler(fIRQ, _InterruptHandler, (void *)this, 0);

	// Enable interesting interrupts now that the handler is in place
	_WriteReg(OHCI_INTERRUPT_ENABLE, OHCI_NORMAL_INTERRUPTS
		| OHCI_MASTER_INTERRUPT_ENABLE);

	TRACE("OHCI host controller driver constructed\n");
	fInitOK = true;
}


OHCI::~OHCI()
{
	int32 result = 0;
	fStopFinishThread = true;
	delete_sem(fFinishTransfersSem);
	wait_for_thread(fFinishThread, &result);

	remove_io_interrupt_handler(fIRQ, _InterruptHandler, (void *)this);

	_LockEndpoints();
	mutex_destroy(&fEndpointLock);

	if (fHccaArea >= B_OK)
		delete_area(fHccaArea);
	if (fRegisterArea >= B_OK)
		delete_area(fRegisterArea);

	_FreeEndpoint(fDummyControl);
	_FreeEndpoint(fDummyBulk);
	_FreeEndpoint(fDummyIsochronous);

	if (fInterruptEndpoints != NULL) {
		for (int i = 0; i < OHCI_STATIC_ENDPOINT_COUNT; i++)
			_FreeEndpoint(fInterruptEndpoints[i]);
	}

	delete [] fFrameBandwidth;
	delete [] fInterruptEndpoints;
	delete fRootHub;

	if (fUseMSI) {
		fPci->disable_msi(fDevice);
		fPci->unconfigure_msi(fDevice);
	}
}


status_t
OHCI::Start()
{
	TRACE("starting OHCI host controller\n");

	uint32 control = _ReadReg(OHCI_CONTROL);
	if ((control & OHCI_HC_FUNCTIONAL_STATE_MASK)
		!= OHCI_HC_FUNCTIONAL_STATE_OPERATIONAL) {
		TRACE_ERROR("controller not started (0x%08" B_PRIx32 ")!\n", control);
		return B_ERROR;
	} else
		TRACE("controller is operational!\n");

	fRootHubAddress = AllocateAddress();
	fRootHub = new(std::nothrow) OHCIRootHub(RootObject(), fRootHubAddress);
	if (!fRootHub) {
		TRACE_ERROR("no memory to allocate root hub\n");
		return B_NO_MEMORY;
	}

	if (fRootHub->InitCheck() < B_OK) {
		TRACE_ERROR("root hub failed init check\n");
		return B_ERROR;
	}

	SetRootHub(fRootHub);

	fRootHub->RegisterNode(Node());

	TRACE_ALWAYS("successfully started the controller\n");
	return BusManager::Start();
}


status_t
OHCI::SubmitTransfer(Transfer *transfer)
{
	// short circuit the root hub
	if (transfer->TransferPipe()->DeviceAddress() == fRootHubAddress)
		return fRootHub->ProcessTransfer(this, transfer);

	uint32 type = transfer->TransferPipe()->Type();
	if (type & USB_OBJECT_CONTROL_PIPE) {
		TRACE("submitting request\n");
		return _SubmitRequest(transfer);
	}

	if ((type & USB_OBJECT_BULK_PIPE) || (type & USB_OBJECT_INTERRUPT_PIPE)) {
		TRACE("submitting %s transfer\n",
			(type & USB_OBJECT_BULK_PIPE) ? "bulk" : "interrupt");
		return _SubmitTransfer(transfer);
	}

	if (type & USB_OBJECT_ISO_PIPE) {
		TRACE("submitting isochronous transfer\n");
		return _SubmitIsochronousTransfer(transfer);
	}

	TRACE_ERROR("tried to submit transfer for unknown pipe type %" B_PRIu32 "\n",
		type);
	return B_ERROR;
}


status_t
OHCI::CancelQueuedTransfers(Pipe *pipe, bool force)
{
	if (!Lock())
		return B_ERROR;

	struct transfer_entry {
		Transfer *			transfer;
		transfer_entry *	next;
	};

	transfer_entry *list = NULL;
	transfer_data *current = fFirstTransfer;
	while (current) {
		if (current->transfer && current->transfer->TransferPipe() == pipe) {
			// Check if the skip bit is already set
			if (!(current->endpoint->flags & OHCI_ENDPOINT_SKIP)) {
				current->endpoint->flags |= OHCI_ENDPOINT_SKIP;
				// In case the controller is processing
				// this endpoint, wait for it to finish
				snooze(1000);
			}

			// Clear the endpoint
			current->endpoint->head_physical_descriptor
				= current->endpoint->tail_physical_descriptor;

			if (pipe->Type() & USB_OBJECT_ISO_PIPE) {
				ohci_isochronous_td *descriptor
					= (ohci_isochronous_td *)current->first_descriptor;
				while (descriptor) {
					uint16 frame = OHCI_ITD_GET_STARTING_FRAME(
						descriptor->flags);
					_ReleaseIsochronousBandwidth(frame,
						OHCI_ITD_GET_FRAME_COUNT(descriptor->flags));
					if (descriptor
							== (ohci_isochronous_td*)current->last_descriptor)
						// this is the last ITD of the transfer
						break;

					descriptor
						= (ohci_isochronous_td *)
						descriptor->next_done_descriptor;
				}
			}

			transfer_entry *entry
				= (transfer_entry *)malloc(sizeof(transfer_entry));
			if (entry != NULL) {
				entry->transfer = current->transfer;
				current->transfer = NULL;
				entry->next = list;
				list = entry;
			}

			current->canceled = true;
		}
		current = current->link;
	}

	Unlock();

	while (list != NULL) {
		transfer_entry *next = list->next;

		// If the transfer is canceled by force, the one causing the
		// cancel is possibly not the one who initiated the transfer
		// and the callback is likely not safe anymore
		if (!force)
			list->transfer->Finished(B_CANCELED, 0);

		delete list->transfer;
		free(list);
		list = next;
	}

	// wait for any transfers that might have made it before canceling
	while (fProcessingPipe == pipe)
		snooze(1000);

	// notify the finisher so it can clean up the canceled transfers
	release_sem_etc(fFinishTransfersSem, 1, B_DO_NOT_RESCHEDULE);
	return B_OK;
}


status_t
OHCI::NotifyPipeChange(Pipe *pipe, usb_change change)
{
	TRACE("pipe change %d for pipe %p\n", change, pipe);
	if (pipe->DeviceAddress() == fRootHubAddress) {
		// no need to insert/remove endpoint descriptors for the root hub
		return B_OK;
	}

	switch (change) {
		case USB_CHANGE_CREATED:
			return _InsertEndpointForPipe(pipe);

		case USB_CHANGE_DESTROYED:
			return _RemoveEndpointForPipe(pipe);

		case USB_CHANGE_PIPE_POLICY_CHANGED:
			TRACE("pipe policy changing unhandled!\n");
			break;

		default:
			TRACE_ERROR("unknown pipe change!\n");
			return B_ERROR;
	}

	return B_OK;
}


status_t
OHCI::GetPortStatus(uint8 index, usb_port_status *status)
{
	if (index >= fPortCount) {
		TRACE_ERROR("get port status for invalid port %u\n", index);
		return B_BAD_INDEX;
	}

	status->status = status->change = 0;
	uint32 portStatus = _ReadReg(OHCI_RH_PORT_STATUS(index));

	// status
	if (portStatus & OHCI_RH_PORTSTATUS_CCS)
		status->status |= PORT_STATUS_CONNECTION;
	if (portStatus & OHCI_RH_PORTSTATUS_PES)
		status->status |= PORT_STATUS_ENABLE;
	if (portStatus & OHCI_RH_PORTSTATUS_PSS)
		status->status |= PORT_STATUS_SUSPEND;
	if (portStatus & OHCI_RH_PORTSTATUS_POCI)
		status->status |= PORT_STATUS_OVER_CURRENT;
	if (portStatus & OHCI_RH_PORTSTATUS_PRS)
		status->status |= PORT_STATUS_RESET;
	if (portStatus & OHCI_RH_PORTSTATUS_PPS)
		status->status |= PORT_STATUS_POWER;
	if (portStatus & OHCI_RH_PORTSTATUS_LSDA)
		status->status |= PORT_STATUS_LOW_SPEED;

	// change
	if (portStatus & OHCI_RH_PORTSTATUS_CSC)
		status->change |= PORT_STATUS_CONNECTION;
	if (portStatus & OHCI_RH_PORTSTATUS_PESC)
		status->change |= PORT_STATUS_ENABLE;
	if (portStatus & OHCI_RH_PORTSTATUS_PSSC)
		status->change |= PORT_STATUS_SUSPEND;
	if (portStatus & OHCI_RH_PORTSTATUS_OCIC)
		status->change |= PORT_STATUS_OVER_CURRENT;
	if (portStatus & OHCI_RH_PORTSTATUS_PRSC)
		status->change |= PORT_STATUS_RESET;

	TRACE("port %u status 0x%04x change 0x%04x\n", index,
		status->status, status->change);
	return B_OK;
}


status_t
OHCI::SetPortFeature(uint8 index, uint16 feature)
{
	TRACE("set port feature index %u feature %u\n", index, feature);
	if (index > fPortCount)
		return B_BAD_INDEX;

	switch (feature) {
		case PORT_ENABLE:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PES);
			return B_OK;

		case PORT_SUSPEND:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PSS);
			return B_OK;

		case PORT_RESET:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PRS);
			return B_OK;

		case PORT_POWER:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PPS);
			return B_OK;
	}

	return B_BAD_VALUE;
}


status_t
OHCI::ClearPortFeature(uint8 index, uint16 feature)
{
	TRACE("clear port feature index %u feature %u\n", index, feature);
	if (index > fPortCount)
		return B_BAD_INDEX;

	switch (feature) {
		case PORT_ENABLE:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_CCS);
			return B_OK;

		case PORT_SUSPEND:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_POCI);
			return B_OK;

		case PORT_POWER:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_LSDA);
			return B_OK;

		case C_PORT_CONNECTION:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_CSC);
			return B_OK;

		case C_PORT_ENABLE:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PESC);
			return B_OK;

		case C_PORT_SUSPEND:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PSSC);
			return B_OK;

		case C_PORT_OVER_CURRENT:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_OCIC);
			return B_OK;

		case C_PORT_RESET:
			_WriteReg(OHCI_RH_PORT_STATUS(index), OHCI_RH_PORTSTATUS_PRSC);
			return B_OK;
	}

	return B_BAD_VALUE;
}


int32
OHCI::_InterruptHandler(void *data)
{
	return ((OHCI *)data)->_Interrupt();
}


int32
OHCI::_Interrupt()
{
	static spinlock lock = B_SPINLOCK_INITIALIZER;
	acquire_spinlock(&lock);

	uint32 status = 0;
	uint32 acknowledge = 0;
	bool finishTransfers = false;
	int32 result = B_HANDLED_INTERRUPT;

	// The LSb of done_head is used to inform the HCD that an interrupt
	// condition exists for both the done list and for another event recorded in
	// the HcInterruptStatus register. If done_head is 0, then the interrupt
	// was caused by other than the HccaDoneHead update and the
	// HcInterruptStatus register needs to be accessed to determine that exact
	// interrupt cause. If HccDoneHead is nonzero, then a done list update
	// interrupt is indicated and if the LSb of the Dword is nonzero, then an
	// additional interrupt event is indicated and HcInterruptStatus should be
	// checked to determine its cause.
	uint32 doneHead = fHcca->done_head;
	if (doneHead != 0) {
		status = OHCI_WRITEBACK_DONE_HEAD;
		if (doneHead & OHCI_DONE_INTERRUPTS)
			status |= _ReadReg(OHCI_INTERRUPT_STATUS)
				& _ReadReg(OHCI_INTERRUPT_ENABLE);
	} else {
		status = _ReadReg(OHCI_INTERRUPT_STATUS) & _ReadReg(OHCI_INTERRUPT_ENABLE)
			& ~OHCI_WRITEBACK_DONE_HEAD;
		if (status == 0) {
			// Nothing to be done (PCI shared interrupt)
			release_spinlock(&lock);
			return B_UNHANDLED_INTERRUPT;
		}
	}

	if (status & OHCI_SCHEDULING_OVERRUN) {
		TRACE_MODULE("scheduling overrun occured\n");
		acknowledge |= OHCI_SCHEDULING_OVERRUN;
	}

	if (status & OHCI_WRITEBACK_DONE_HEAD) {
		TRACE_MODULE("transfer descriptors processed\n");
		fHcca->done_head = 0;
		acknowledge |= OHCI_WRITEBACK_DONE_HEAD;
		result = B_INVOKE_SCHEDULER;
		finishTransfers = true;
	}

	if (status & OHCI_RESUME_DETECTED) {
		TRACE_MODULE("resume detected\n");
		acknowledge |= OHCI_RESUME_DETECTED;
	}

	if (status & OHCI_UNRECOVERABLE_ERROR) {
		TRACE_MODULE_ERROR("unrecoverable error - controller halted\n");
		_WriteReg(OHCI_CONTROL, OHCI_HC_FUNCTIONAL_STATE_RESET);
		// TODO: clear all pending transfers, reset and resetup the controller
	}

	if (status & OHCI_ROOT_HUB_STATUS_CHANGE) {
		TRACE_MODULE("root hub status change\n");
		// Disable the interrupt as it will otherwise be retriggered until the
		// port has been reset and the change is cleared explicitly.
		// TODO: renable it once we use status changes instead of polling
		_WriteReg(OHCI_INTERRUPT_DISABLE, OHCI_ROOT_HUB_STATUS_CHANGE);
		acknowledge |= OHCI_ROOT_HUB_STATUS_CHANGE;
	}

	if (acknowledge != 0)
		_WriteReg(OHCI_INTERRUPT_STATUS, acknowledge);

	release_spinlock(&lock);

	if (finishTransfers)
		release_sem_etc(fFinishTransfersSem, 1, B_DO_NOT_RESCHEDULE);

	return result;
}


status_t
OHCI::_AddPendingTransfer(Transfer *transfer,
	ohci_endpoint_descriptor *endpoint, ohci_general_td *firstDescriptor,
	ohci_general_td *dataDescriptor, ohci_general_td *lastDescriptor,
	bool directionIn)
{
	if (!transfer || !endpoint || !lastDescriptor)
		return B_BAD_VALUE;

	transfer_data *data = new(std::nothrow) transfer_data;
	if (!data)
		return B_NO_MEMORY;

	status_t result = transfer->InitKernelAccess();
	if (result < B_OK) {
		delete data;
		return result;
	}

	data->transfer = transfer;
	data->endpoint = endpoint;
	data->incoming = directionIn;
	data->canceled = false;
	data->link = NULL;

	// the current tail will become the first descriptor
	data->first_descriptor = (ohci_general_td *)endpoint->tail_logical_descriptor;

	// the data and first descriptors might be the same
	if (dataDescriptor == firstDescriptor)
		data->data_descriptor = data->first_descriptor;
	else
		data->data_descriptor = dataDescriptor;

	// even the last and the first descriptor might be the same
	if (lastDescriptor == firstDescriptor)
		data->last_descriptor = data->first_descriptor;
	else
		data->last_descriptor = lastDescriptor;

	if (!Lock()) {
		delete data;
		return B_ERROR;
	}

	if (fLastTransfer)
		fLastTransfer->link = data;
	else
		fFirstTransfer = data;

	fLastTransfer = data;
	Unlock();

	return B_OK;
}


status_t
OHCI::_AddPendingIsochronousTransfer(Transfer *transfer,
	ohci_endpoint_descriptor *endpoint, ohci_isochronous_td *firstDescriptor,
	ohci_isochronous_td *lastDescriptor, bool directionIn)
{
	if (!transfer || !endpoint || !lastDescriptor)
		return B_BAD_VALUE;

	transfer_data *data = new(std::nothrow) transfer_data;
	if (!data)
		return B_NO_MEMORY;

	status_t result = transfer->InitKernelAccess();
	if (result < B_OK) {
		delete data;
		return result;
	}

	data->transfer = transfer;
	data->endpoint = endpoint;
	data->incoming = directionIn;
	data->canceled = false;
	data->link = NULL;

	// the current tail will become the first descriptor
	data->first_descriptor = (ohci_general_td*)endpoint->tail_logical_descriptor;

	// the data and first descriptors are the same
	data->data_descriptor = data->first_descriptor;

	// the last and the first descriptor might be the same
	if (lastDescriptor == firstDescriptor)
		data->last_descriptor = data->first_descriptor;
	else
		data->last_descriptor = (ohci_general_td*)lastDescriptor;

	if (!Lock()) {
		delete data;
		return B_ERROR;
	}

	if (fLastTransfer)
		fLastTransfer->link = data;
	else
		fFirstTransfer = data;

	fLastTransfer = data;
	Unlock();

	return B_OK;
}


int32
OHCI::_FinishThread(void *data)
{
	((OHCI *)data)->_FinishTransfers();
	return B_OK;
}


void
OHCI::_FinishTransfers()
{
	while (!fStopFinishThread) {
		if (acquire_sem(fFinishTransfersSem) < B_OK)
			continue;

		// eat up sems that have been released by multiple interrupts
		int32 semCount = 0;
		get_sem_count(fFinishTransfersSem, &semCount);
		if (semCount > 0)
			acquire_sem_etc(fFinishTransfersSem, semCount, B_RELATIVE_TIMEOUT, 0);

		if (!Lock())
			continue;

		TRACE("finishing transfers (first transfer: %p; last"
			" transfer: %p)\n", fFirstTransfer, fLastTransfer);
		transfer_data *lastTransfer = NULL;
		transfer_data *transfer = fFirstTransfer;
		Unlock();

		while (transfer) {
			bool transferDone = false;
			ohci_general_td *descriptor = transfer->first_descriptor;
			ohci_endpoint_descriptor *endpoint = transfer->endpoint;
			status_t callbackStatus = B_OK;

			if (endpoint->flags & OHCI_ENDPOINT_ISOCHRONOUS_FORMAT) {
				transfer_data *next = transfer->link;
				if (_FinishIsochronousTransfer(transfer, &lastTransfer)) {
					delete transfer->transfer;
					delete transfer;
				}
				transfer = next;
				continue;
			}

			MutexLocker endpointLocker(endpoint->lock);

			if ((endpoint->head_physical_descriptor & OHCI_ENDPOINT_HEAD_MASK)
					!= endpoint->tail_physical_descriptor
						&& (endpoint->head_physical_descriptor
							& OHCI_ENDPOINT_HALTED) == 0) {
				// there are still active transfers on this endpoint, we need
				// to wait for all of them to complete, otherwise we'd read
				// a potentially bogus data toggle value below
				TRACE("endpoint %p still has active tds\n", endpoint);
				lastTransfer = transfer;
				transfer = transfer->link;
				continue;
			}

			endpointLocker.Unlock();

			while (descriptor && !transfer->canceled) {
				uint32 status = OHCI_TD_GET_CONDITION_CODE(descriptor->flags);
				if (status == OHCI_TD_CONDITION_NOT_ACCESSED) {
					// td is still active
					TRACE("td %p still active\n", descriptor);
					break;
				}

				if (status != OHCI_TD_CONDITION_NO_ERROR) {
					// an error occured, but we must ensure that the td
					// was actually done
					if (endpoint->head_physical_descriptor & OHCI_ENDPOINT_HALTED) {
						// the endpoint is halted, this guaratees us that this
						// descriptor has passed (we don't know if the endpoint
						// was halted because of this td, but we do not need
						// to know, as when it was halted by another td this
						// still ensures that this td was handled before).
						TRACE_ERROR("td error: 0x%08" B_PRIx32 "\n", status);

						callbackStatus = _GetStatusOfConditionCode(status);

						transferDone = true;
						break;
					} else {
						// an error occured but the endpoint is not halted so
						// the td is in fact still active
						TRACE("td %p active with error\n", descriptor);
						break;
					}
				}

				// the td has completed without an error
				TRACE("td %p done\n", descriptor);

				if (descriptor == transfer->last_descriptor
					|| descriptor->buffer_physical != 0) {
					// this is the last td of the transfer or a short packet
					callbackStatus = B_OK;
					transferDone = true;
					break;
				}

				descriptor
					= (ohci_general_td *)descriptor->next_logical_descriptor;
			}

			if (transfer->canceled) {
				// when a transfer is canceled, all transfers to that endpoint
				// are canceled by setting the head pointer to the tail pointer
				// which causes all of the tds to become "free" (as they are
				// inaccessible and not accessed anymore (as setting the head
				// pointer required disabling the endpoint))
				callbackStatus = B_OK;
				transferDone = true;
			}

			if (!transferDone) {
				lastTransfer = transfer;
				transfer = transfer->link;
				continue;
			}

			// remove the transfer from the list first so we are sure
			// it doesn't get canceled while we still process it
			transfer_data *next = transfer->link;
			if (Lock()) {
				if (lastTransfer)
					lastTransfer->link = transfer->link;

				if (transfer == fFirstTransfer)
					fFirstTransfer = transfer->link;
				if (transfer == fLastTransfer)
					fLastTransfer = lastTransfer;

				// store the currently processing pipe here so we can wait
				// in cancel if we are processing something on the target pipe
				if (!transfer->canceled)
					fProcessingPipe = transfer->transfer->TransferPipe();

				transfer->link = NULL;
				Unlock();
			}

			// break the descriptor chain on the last descriptor
			transfer->last_descriptor->next_logical_descriptor = NULL;
			TRACE("transfer %p done with status 0x%08" B_PRIx32 "\n",
				transfer, callbackStatus);

			// if canceled the callback has already been called
			if (!transfer->canceled) {
				size_t actualLength = 0;
				if (callbackStatus == B_OK) {
					if (transfer->data_descriptor && transfer->incoming) {
						// data to read out
						generic_io_vec *vector = transfer->transfer->Vector();
						size_t vectorCount = transfer->transfer->VectorCount();

						transfer->transfer->PrepareKernelAccess();
						actualLength = _ReadDescriptorChain(
							transfer->data_descriptor,
							vector, vectorCount, transfer->transfer->IsPhysical());
					} else if (transfer->data_descriptor) {
						// read the actual length that was sent
						actualLength = _ReadActualLength(
							transfer->data_descriptor);
					}

					// get the last data toggle and store it for next time
					transfer->transfer->TransferPipe()->SetDataToggle(
						(endpoint->head_physical_descriptor
							& OHCI_ENDPOINT_TOGGLE_CARRY) != 0);

					if (transfer->transfer->IsFragmented()) {
						// this transfer may still have data left
						TRACE("advancing fragmented transfer\n");
						transfer->transfer->AdvanceByFragment(actualLength);
						if (transfer->transfer->FragmentLength() > 0) {
							TRACE("still %ld bytes left on transfer\n",
								transfer->transfer->FragmentLength());
							// TODO actually resubmit the transfer
						}

						// the transfer is done, but we already set the
						// actualLength with AdvanceByFragment()
						actualLength = 0;
					}
				}

				transfer->transfer->Finished(callbackStatus, actualLength);
				fProcessingPipe = NULL;
			}

			if (callbackStatus != B_OK) {
				// remove the transfer and make the head pointer valid again
				// (including clearing the halt state)
				_RemoveTransferFromEndpoint(transfer);
			}

			// free the descriptors
			_FreeDescriptorChain(transfer->first_descriptor);

			delete transfer->transfer;
			delete transfer;
			transfer = next;
		}
	}
}


bool
OHCI::_FinishIsochronousTransfer(transfer_data *transfer,
	transfer_data **_lastTransfer)
{
	status_t callbackStatus = B_OK;
	size_t actualLength = 0;
	uint32 packet = 0;

	/*	Unlink BEFORE touching transfer->transfer.

		This panics the machine under an ordinary shutdown with an isochronous
		stream still live. The old code did all of
		the descriptor processing FIRST and only unlinked the transfer from
		fFirstTransfer afterwards -- despite the comment on that block saying
		"remove the transfer from the list first so we are sure it doesn't get
		canceled while we still process it". The general-TD path in
		_FinishTransfers() really does unlink first (:1249-1265, before it
		touches transfer->transfer at :1273); only the isochronous path had the
		order inverted, which is why only this one crashed.

		THE RACE. CancelQueuedTransfers() nulls the pointer before it raises the
		flag, both under the OHCI lock:

			:739  current->transfer = NULL;
			:744  current->canceled = true;

		while this function tested transfer->canceled and then dereferenced
		transfer->transfer with NO lock held. So the finish thread could read
		canceled == false, be preempted, and dereference a pointer cancellation
		had since nulled. Observed exactly: a live transfer_data with
		transfer == NULL and canceled == true, faulting on
		transfer->transfer->IsochronousData().

		WHY UNLINKING IS THE FIX, and why it is sufficient. Cancellation reaches
		a transfer only by walking fFirstTransfer (:698-747). Once we have taken
		it off that list under the lock, cancellation cannot find it, cannot null
		its Transfer, and cannot delete it (:760) -- so everything after this
		point owns the transfer exclusively and needs no further synchronisation.

		Reading `canceled` under the SAME lock acquisition is the other half.
		Cancel performs both stores while holding the lock, so any lock holder
		sees a consistent pair: either (transfer != NULL, canceled == false) or
		(transfer == NULL, canceled == true). Never the torn state that crashed.

		⚠️ Reordering cancel's two stores, or adding a NULL check here, would
		only narrow the window -- both fields are read unlocked and cancel goes
		on to delete the Transfer, so even a snapshot can be freed underneath.
		Narrowing is not fixing.
	*/
	bool canceled = transfer->canceled;

	// Declared out here so the retirement walk below decides whether the
	// processing loop runs at all: it leaves this at first_descriptor when it
	// found last_descriptor, and NULL when the chain ran out without one. Stock
	// relied on the same variable carrying that meaning across, and a malformed
	// chain must still be skipped rather than walked.
	ohci_isochronous_td *descriptor = NULL;

	if (!canceled) {
		// at first check if ALL ITDs are retired by HC. Done before unlinking:
		// a transfer that is not finished has to stay on the list.
		descriptor = (ohci_isochronous_td *)transfer->first_descriptor;
		while (descriptor) {
			if (OHCI_TD_GET_CONDITION_CODE(descriptor->flags)
				== OHCI_TD_CONDITION_NOT_ACCESSED) {
				TRACE("ITD %p still active\n", descriptor);
				*_lastTransfer = transfer;
				return false;
			}

			if (descriptor == (ohci_isochronous_td*)transfer->last_descriptor) {
				// this is the last ITD of the transfer
				descriptor = (ohci_isochronous_td *)transfer->first_descriptor;
				break;
			}

			descriptor
				= (ohci_isochronous_td *)descriptor->next_done_descriptor;
		}
	}

	/*	UNLINK UNCONDITIONALLY, CANCELED INCLUDED.

		This block used to sit INSIDE the `if (!canceled)` above, which f859bbf
		introduced while moving it ahead of the transfer->transfer dereferences.
		That move was right; putting it inside the branch was not.

		The caller frees the transfer whatever we return true for:

			:1157  if (endpoint->flags & OHCI_ENDPOINT_ISOCHRONOUS_FORMAT) {
			:1159      if (_FinishIsochronousTransfer(transfer, &lastTransfer)) {
			:1160          delete transfer->transfer;
			:1161          delete transfer;

		and it does NOT unlink -- that is this function's job. So a CANCELED
		transfer had its descriptor chain freed (:1603, unconditional) and its
		transfer_data deleted, while fFirstTransfer / the predecessor's link
		still pointed at it. The next _FinishTransfers() pass then walked freed
		memory and freed the same ITD chain a second time, which is what
		produced the run of

			PMA: address was not allocated!

		during shutdown with an isochronous stream active (16 of them on
		the unlink change above). That message is the benign half: the guard in
		PhysicalMemoryAllocator::Deallocate refuses the second free. The
		use-after-free of the transfer_data itself is not guarded by anything.

		Stock is correct here and always was -- its unlink is unconditional
		(compare ~/repos/haiku ohci.cpp, where `if (transfer->canceled)` only
		selects the callback status and the Lock() block sits outside it). This
		restores that, keeping f859bbf's ordering: the retirement walk still
		runs first and can still `return false` to leave an unfinished transfer
		on the list, and fProcessingPipe stays guarded because it is the one
		thing here that dereferences transfer->transfer -- which the cancel path
		has already NULLed.
	*/
	if (Lock()) {
		if (*_lastTransfer)
			(*_lastTransfer)->link = transfer->link;

		if (transfer == fFirstTransfer)
			fFirstTransfer = transfer->link;
		if (transfer == fLastTransfer)
			fLastTransfer = *_lastTransfer;

		canceled = transfer->canceled;

		// store the currently processing pipe here so we can wait
		// in cancel if we are processing something on the target pipe
		if (!canceled)
			fProcessingPipe = transfer->transfer->TransferPipe();

		transfer->link = NULL;
		Unlock();
	}

	if (canceled)
		callbackStatus = B_CANCELED;
	else {
		while (descriptor) {
			uint32 status = OHCI_TD_GET_CONDITION_CODE(descriptor->flags);
			if (status != OHCI_TD_CONDITION_NO_ERROR) {
				TRACE_ERROR("ITD error: 0x%08" B_PRIx32 "\n", status);
				// spec says that in most cases condition code
				// of retired ITDs is set to NoError, but for the
				// time overrun it can be DataOverrun. We assume
				// the _first_ occurience of such error as status
				// reported to the callback
				if (callbackStatus == B_OK)
					callbackStatus = _GetStatusOfConditionCode(status);
			}

			usb_isochronous_data *isochronousData
				= transfer->transfer->IsochronousData();

			uint32 frameCount = OHCI_ITD_GET_FRAME_COUNT(descriptor->flags);
			for (size_t i = 0; i < frameCount; i++, packet++) {
				usb_iso_packet_descriptor* packet_descriptor
					= &isochronousData->packet_descriptors[packet];

				uint16 offset = descriptor->offset[OHCI_ITD_OFFSET_IDX(i)];
				uint8 code = OHCI_ITD_GET_BUFFER_CONDITION_CODE(offset);
				packet_descriptor->status = _GetStatusOfConditionCode(code);

				/*	Recognise BOTH NotAccessed encodings.

					OHCI 1.0a 4.3.2.3.5.3 defines both 1110b and 1111b as
					NotAccessed in a packet status word, but
					_GetStatusOfConditionCode() maps 0x0F to B_DEV_PENDING --
					correct for a general TD, where NotAccessed means "still
					active" -- and only 0x0E to B_DEV_TOO_LATE. Stock skipped
					B_DEV_TOO_LATE alone, so a NotAccessed frame reading 0x0F
					fell through to the length calculation below and had its
					OFFSET bits consumed as a transferred byte count.

					Which of the two an untouched frame carries is decided by
					bit 12 of the offset word -- the buffer page selector, which
					OHCI_ITD_MK_OFFS() ORs into the same nibble the condition
					code occupies. So a frame starting past the ITD's first page
					reads 0x0F, and one below it reads 0x0E.

					Not reachable on the DDJ-SR -- its largest ITD is 4224 bytes
					with offsets up to 3696, so bit 12 is never set -- but a
					full-speed endpoint with packets near the 1023-byte maximum
					crosses 4096 inside a single 8-frame ITD and does hit it.

					actual_length is zeroed here too. Stock left the previous
					transfer's value in place on this skip path, and the
					descriptor array is reused every buffer, so a stale count
					could be read back as real data for a frame that never
					happened.
				*/
				if (packet_descriptor->status == B_DEV_TOO_LATE
					|| packet_descriptor->status == B_DEV_PENDING) {
					packet_descriptor->status = B_DEV_TOO_LATE;
					packet_descriptor->actual_length = 0;
					continue;
				}

				size_t len = OHCI_ITD_GET_BUFFER_LENGTH(offset);
				if (!transfer->incoming)
					len = packet_descriptor->request_length - len;

				packet_descriptor->actual_length = len;
				actualLength += len;
			}

			uint16 frame = OHCI_ITD_GET_STARTING_FRAME(descriptor->flags);
			// This is the hot path -- it runs on every completed ITD of
			// a running stream, which is why the stock reset-to-MAX kept the
			// accounting permanently flatlined rather than only leaking at
			// teardown.
			_ReleaseIsochronousBandwidth(frame,
				OHCI_ITD_GET_FRAME_COUNT(descriptor->flags));

			TRACE("ITD %p done\n", descriptor);

			if (descriptor == (ohci_isochronous_td*)transfer->last_descriptor)
				break;

			descriptor
				= (ohci_isochronous_td *)descriptor->next_done_descriptor;
		}
	}

	// (The unlink that used to sit here has moved ABOVE, before anything
	// dereferences transfer->transfer -- see the race note at the top of this
	// function. Doing it here was the bug.)

	/*	Break the chain through the RIGHT type.

		STOCK BUG: transfer_data::last_descriptor is declared ohci_general_td*,
		but the isochronous path stores an ohci_isochronous_td* in it
		(_AddPendingIsochronousTransfer casts it in). Stock then wrote

			transfer->last_descriptor->next_logical_descriptor = NULL;

		through that wrong type. The two structures do not share a layout --
		the ITD carries a 16-byte offset[8] PSW array the general TD has no
		equivalent for -- so the field this lands on is not the one intended.
		Measured with offsetof() on x86_64:

			ohci_general_td::next_logical_descriptor  @ 40
			ohci_isochronous_td::buffer_size          @ 40   <-- collision
			ohci_isochronous_td::next_logical_descriptor @ 56

		So every completing isochronous transfer silently zeroed its LAST
		descriptor's buffer_size. _FreeIsochronousDescriptor() then called
		FreeChunk(buffer_logical, ..., 0), and PhysicalMemoryAllocator::
		Deallocate() rejects a zero size before touching its bitmaps -- the
		block is never returned and the buddy tree keeps it marked forever.

		WHY IT LOOKED LIKE A CONTROLLER BUG. It leaks exactly one buffer per
		transfer, the trailing ITD's, while the first ITD's buffer travels to
		the endpoint's old tail object and is freed correctly. Measured over
		one 24 s run of the DDJ-SR at 44.1 kHz:

			PMA[ 7] block 1024  alloc 2370  freed    0   <- 2nd ITD (capture)
			PMA[ 8] block 2048  alloc 2371  freed    0   <- 2nd ITD (playback)
			PMA[ 9] block 4096  alloc 2370  freed 2368   <- 1st ITD, fine
			PMA[10] block 8192  alloc 2371  freed 2369   <- 1st ITD, fine
			PMA rejected frees: zero-size 4737, bad-index 0,
			                    not-allocated 0, no-address 0

		4737 rejections + 4737 successes = 9474, exactly the number of frees
		OHCI issued. That fills the 8 MB USB pool in ~24 s, after which no
		4224-byte ITD buffer can be allocated, the queue fails, and every
		later transfer time-overruns (ITD condition 0x8, len 0) -- which is
		what reached the ear as "clean for 25 seconds, then dropouts".

		Nothing but isochronous is affected: the general-TD path stores a real
		ohci_general_td* and this line was always correct for it. But the pool
		is shared by every host controller, so the exhaustion is not confined
		to OHCI once it starts.
	*/
	((ohci_isochronous_td *)transfer->last_descriptor)
		->next_logical_descriptor = NULL;
	TRACE("iso.transfer %p done with status 0x%08" B_PRIx32 " len:%ld\n",
		transfer, callbackStatus, actualLength);

	// If canceled the callback has already been called, and transfer->transfer
	// belongs to the cancel path -- use the snapshot taken under the lock, not
	// a fresh unlocked read of transfer->canceled.
	if (!canceled) {
		if (callbackStatus == B_OK && actualLength > 0) {
			if (transfer->data_descriptor && transfer->incoming) {
				// data to read out
				generic_io_vec *vector = transfer->transfer->Vector();
				size_t vectorCount = transfer->transfer->VectorCount();

				transfer->transfer->PrepareKernelAccess();
				_ReadIsochronousDescriptorChain(
					(ohci_isochronous_td*)transfer->data_descriptor,
					vector, vectorCount, transfer->transfer->IsPhysical());
			}
		}

		transfer->transfer->Finished(callbackStatus, actualLength);
		fProcessingPipe = NULL;
	}

	_FreeIsochronousDescriptorChain(
		(ohci_isochronous_td*)transfer->first_descriptor);

	return true;
}


status_t
OHCI::_SubmitRequest(Transfer *transfer)
{
	usb_request_data *requestData = transfer->RequestData();
	bool directionIn = (requestData->RequestType & USB_REQTYPE_DEVICE_IN) != 0;

	ohci_general_td *setupDescriptor
		= _CreateGeneralDescriptor(sizeof(usb_request_data));
	if (!setupDescriptor) {
		TRACE_ERROR("failed to allocate setup descriptor\n");
		return B_NO_MEMORY;
	}

	setupDescriptor->flags = OHCI_TD_DIRECTION_PID_SETUP
		| OHCI_TD_SET_CONDITION_CODE(OHCI_TD_CONDITION_NOT_ACCESSED)
		| OHCI_TD_TOGGLE_0
		| OHCI_TD_SET_DELAY_INTERRUPT(OHCI_TD_INTERRUPT_NONE);

	ohci_general_td *statusDescriptor = _CreateGeneralDescriptor(0);
	if (!statusDescriptor) {
		TRACE_ERROR("failed to allocate status descriptor\n");
		_FreeGeneralDescriptor(setupDescriptor);
		return B_NO_MEMORY;
	}

	statusDescriptor->flags
		= (directionIn ? OHCI_TD_DIRECTION_PID_OUT : OHCI_TD_DIRECTION_PID_IN)
		| OHCI_TD_SET_CONDITION_CODE(OHCI_TD_CONDITION_NOT_ACCESSED)
		| OHCI_TD_TOGGLE_1
		| OHCI_TD_SET_DELAY_INTERRUPT(OHCI_TD_INTERRUPT_IMMEDIATE);

	generic_io_vec vector;
	vector.base = (generic_addr_t)requestData;
	vector.length = sizeof(usb_request_data);
	_WriteDescriptorChain(setupDescriptor, &vector, 1, false);

	status_t result;
	ohci_general_td *dataDescriptor = NULL;
	if (transfer->VectorCount() > 0) {
		ohci_general_td *lastDescriptor = NULL;
		result = _CreateDescriptorChain(&dataDescriptor, &lastDescriptor,
			directionIn ? OHCI_TD_DIRECTION_PID_IN : OHCI_TD_DIRECTION_PID_OUT,
			transfer->FragmentLength());
		if (result < B_OK) {
			_FreeGeneralDescriptor(setupDescriptor);
			_FreeGeneralDescriptor(statusDescriptor);
			return result;
		}

		if (!directionIn) {
			_WriteDescriptorChain(dataDescriptor, transfer->Vector(),
				transfer->VectorCount(), transfer->IsPhysical());
		}

		_LinkDescriptors(setupDescriptor, dataDescriptor);
		_LinkDescriptors(lastDescriptor, statusDescriptor);
	} else {
		_LinkDescriptors(setupDescriptor, statusDescriptor);
	}

	// Add to the transfer list
	ohci_endpoint_descriptor *endpoint
		= (ohci_endpoint_descriptor *)transfer->TransferPipe()->ControllerCookie();

	MutexLocker endpointLocker(endpoint->lock);
	result = _AddPendingTransfer(transfer, endpoint, setupDescriptor,
		dataDescriptor, statusDescriptor, directionIn);
	if (result < B_OK) {
		TRACE_ERROR("failed to add pending transfer\n");
		_FreeDescriptorChain(setupDescriptor);
		return result;
	}

	// Add the descriptor chain to the endpoint
	_SwitchEndpointTail(endpoint, setupDescriptor, statusDescriptor);
	endpointLocker.Unlock();

	// Tell the controller to process the control list
	endpoint->flags &= ~OHCI_ENDPOINT_SKIP;
	_WriteReg(OHCI_COMMAND_STATUS, OHCI_CONTROL_LIST_FILLED);
	return B_OK;
}


status_t
OHCI::_SubmitTransfer(Transfer *transfer)
{
	Pipe *pipe = transfer->TransferPipe();
	bool directionIn = (pipe->Direction() == Pipe::In);

	ohci_general_td *firstDescriptor = NULL;
	ohci_general_td *lastDescriptor = NULL;
	status_t result = _CreateDescriptorChain(&firstDescriptor, &lastDescriptor,
		directionIn ? OHCI_TD_DIRECTION_PID_IN : OHCI_TD_DIRECTION_PID_OUT,
		transfer->FragmentLength());

	if (result < B_OK)
		return result;

	// Apply data toggle to the first descriptor (the others will use the carry)
	firstDescriptor->flags &= ~OHCI_TD_TOGGLE_CARRY;
	firstDescriptor->flags |= pipe->DataToggle() ? OHCI_TD_TOGGLE_1
		: OHCI_TD_TOGGLE_0;

	// Set the last descriptor to generate an interrupt
	lastDescriptor->flags &= ~OHCI_TD_INTERRUPT_MASK;
	lastDescriptor->flags |=
		OHCI_TD_SET_DELAY_INTERRUPT(OHCI_TD_INTERRUPT_IMMEDIATE);

	if (!directionIn) {
		_WriteDescriptorChain(firstDescriptor, transfer->Vector(),
			transfer->VectorCount(), transfer->IsPhysical());
	}

	// Add to the transfer list
	ohci_endpoint_descriptor *endpoint
		= (ohci_endpoint_descriptor *)pipe->ControllerCookie();

	MutexLocker endpointLocker(endpoint->lock);

	// We do not support queuing other transfers in tandem with a fragmented one.
	transfer_data *it = fFirstTransfer;
	while (it) {
		if (it->transfer && it->transfer->TransferPipe() == pipe && it->transfer->IsFragmented()) {
			TRACE_ERROR("cannot submit transfer: a fragmented transfer is queued\n");
			_FreeDescriptorChain(firstDescriptor);
			return B_DEV_RESOURCE_CONFLICT;
		}

		it = it->link;
	}

	result = _AddPendingTransfer(transfer, endpoint, firstDescriptor,
		firstDescriptor, lastDescriptor, directionIn);
	if (result < B_OK) {
		TRACE_ERROR("failed to add pending transfer\n");
		_FreeDescriptorChain(firstDescriptor);
		return result;
	}

	// Add the descriptor chain to the endpoint
	_SwitchEndpointTail(endpoint, firstDescriptor, lastDescriptor);
	endpointLocker.Unlock();

	endpoint->flags &= ~OHCI_ENDPOINT_SKIP;
	if (pipe->Type() & USB_OBJECT_BULK_PIPE) {
		// Tell the controller to process the bulk list
		_WriteReg(OHCI_COMMAND_STATUS, OHCI_BULK_LIST_FILLED);
	}

	return B_OK;
}


status_t
OHCI::_SubmitIsochronousTransfer(Transfer *transfer)
{
	Pipe *pipe = transfer->TransferPipe();
	bool directionIn = (pipe->Direction() == Pipe::In);

	ohci_isochronous_td *firstDescriptor = NULL;
	ohci_isochronous_td *lastDescriptor = NULL;
	status_t result = _CreateIsochronousDescriptorChain(&firstDescriptor,
		&lastDescriptor, transfer);

	if (firstDescriptor == 0 || lastDescriptor == 0)
		return B_ERROR;

	if (result < B_OK)
		return result;

	// Set the last descriptor to generate an interrupt
	lastDescriptor->flags &= ~OHCI_ITD_INTERRUPT_MASK;
	// let the controller retire last ITD
	lastDescriptor->flags |= OHCI_ITD_SET_DELAY_INTERRUPT(1);

	// If direction is out set every descriptor data
	if (pipe->Direction() == Pipe::Out)
		_WriteIsochronousDescriptorChain(firstDescriptor,
			transfer->Vector(), transfer->VectorCount(), transfer->IsPhysical());

	// Add to the transfer list
	ohci_endpoint_descriptor *endpoint
		= (ohci_endpoint_descriptor *)pipe->ControllerCookie();

	MutexLocker endpointLocker(endpoint->lock);
	result = _AddPendingIsochronousTransfer(transfer, endpoint,
		firstDescriptor, lastDescriptor, directionIn);
	if (result < B_OK) {
		TRACE_ERROR("failed to add pending iso.transfer:"
			"0x%08" B_PRIx32 "\n", result);
		_FreeIsochronousDescriptorChain(firstDescriptor);
		return result;
	}

	// Add the descriptor chain to the endpoint
	_SwitchIsochronousEndpointTail(endpoint, firstDescriptor, lastDescriptor);
	endpointLocker.Unlock();

	endpoint->flags &= ~OHCI_ENDPOINT_SKIP;

	return B_OK;
}


void
OHCI::_SwitchEndpointTail(ohci_endpoint_descriptor *endpoint,
	ohci_general_td *first, ohci_general_td *last)
{
	// fill in the information of the first descriptor into the current tail
	ohci_general_td *tail = (ohci_general_td *)endpoint->tail_logical_descriptor;
	tail->flags = first->flags;
	tail->buffer_physical = first->buffer_physical;
	tail->next_physical_descriptor = first->next_physical_descriptor;
	tail->last_physical_byte_address = first->last_physical_byte_address;
	tail->buffer_size = first->buffer_size;
	tail->buffer_logical = first->buffer_logical;
	tail->next_logical_descriptor = first->next_logical_descriptor;

	// the first descriptor becomes the new tail
	first->flags = 0;
	first->buffer_physical = 0;
	first->next_physical_descriptor = 0;
	first->last_physical_byte_address = 0;
	first->buffer_size = 0;
	first->buffer_logical = NULL;
	first->next_logical_descriptor = NULL;

	if (first == last)
		_LinkDescriptors(tail, first);
	else
		_LinkDescriptors(last, first);

	// update the endpoint tail pointer to reflect the change
	endpoint->tail_logical_descriptor = first;
	endpoint->tail_physical_descriptor = (uint32)first->physical_address;
	TRACE("switched tail from %p to %p\n", tail, first);

#if 0
	_PrintEndpoint(endpoint);
	_PrintDescriptorChain(tail);
#endif
}


void
OHCI::_SwitchIsochronousEndpointTail(ohci_endpoint_descriptor *endpoint,
	ohci_isochronous_td *first, ohci_isochronous_td *last)
{
	// fill in the information of the first descriptor into the current tail
	ohci_isochronous_td *tail
		= (ohci_isochronous_td*)endpoint->tail_logical_descriptor;
	tail->flags = first->flags;
	tail->buffer_page_byte_0 = first->buffer_page_byte_0;
	tail->next_physical_descriptor = first->next_physical_descriptor;
	tail->last_byte_address = first->last_byte_address;
	tail->buffer_size = first->buffer_size;
	tail->buffer_logical = first->buffer_logical;
	tail->next_logical_descriptor = first->next_logical_descriptor;
	tail->next_done_descriptor = first->next_done_descriptor;

	// the first descriptor becomes the new tail
	first->flags = 0;
	first->buffer_page_byte_0 = 0;
	first->next_physical_descriptor = 0;
	first->last_byte_address = 0;
	first->buffer_size = 0;
	first->buffer_logical = NULL;
	first->next_logical_descriptor = NULL;
	first->next_done_descriptor = NULL;

	for (int i = 0; i < OHCI_ITD_NOFFSET; i++) {
		tail->offset[i] = first->offset[i];
		first->offset[i] = 0;
	}

	if (first == last)
		_LinkIsochronousDescriptors(tail, first, NULL);
	else
		_LinkIsochronousDescriptors(last, first, NULL);

	// update the endpoint tail pointer to reflect the change
	endpoint->tail_logical_descriptor = first;
	endpoint->tail_physical_descriptor = (uint32)first->physical_address;
	TRACE("switched tail from %p to %p\n", tail, first);

#if 0
	_PrintEndpoint(endpoint);
	_PrintDescriptorChain(tail);
#endif
}


void
OHCI::_RemoveTransferFromEndpoint(transfer_data *transfer)
{
	// The transfer failed and the endpoint was halted. This means that the
	// endpoint head pointer might point somewhere into the descriptor chain
	// of this transfer. As we do not know if this transfer actually caused
	// the halt on the endpoint we have to make sure this is the case. If we
	// find the head to point to somewhere into the descriptor chain then
	// simply advancing the head pointer to the link of the last transfer
	// will bring the endpoint into a valid state again. This operation is
	// safe as the endpoint is currently halted and we therefore can change
	// the head pointer.
	ohci_endpoint_descriptor *endpoint = transfer->endpoint;
	ohci_general_td *descriptor = transfer->first_descriptor;
	while (descriptor) {
		if ((endpoint->head_physical_descriptor & OHCI_ENDPOINT_HEAD_MASK)
			== descriptor->physical_address) {
			// This descriptor caused the halt. Advance the head pointer. This
			// will either move the head to the next valid transfer that can
			// then be restarted, or it will move the head to the tail when
			// there are no more transfer descriptors. Setting the head will
			// also clear the halt state as it is stored in the first bit of
			// the head pointer.
			endpoint->head_physical_descriptor
				= transfer->last_descriptor->next_physical_descriptor;
			return;
		}

		descriptor = (ohci_general_td *)descriptor->next_logical_descriptor;
	}
}


ohci_endpoint_descriptor *
OHCI::_AllocateEndpoint()
{
	ohci_endpoint_descriptor *endpoint;
	phys_addr_t physicalAddress;

	mutex *lock = (mutex *)malloc(sizeof(mutex));
	if (lock == NULL) {
		TRACE_ERROR("no memory to allocate endpoint lock\n");
		return NULL;
	}

	// Allocate memory chunk
	if (fStack->AllocateChunk((void **)&endpoint, &physicalAddress,
		sizeof(ohci_endpoint_descriptor)) < B_OK) {
		TRACE_ERROR("failed to allocate endpoint descriptor\n");
		free(lock);
		return NULL;
	}

	mutex_init(lock, "ohci endpoint lock");

	endpoint->flags = OHCI_ENDPOINT_SKIP;
	endpoint->physical_address = (uint32)physicalAddress;
	endpoint->head_physical_descriptor = 0;
	endpoint->tail_logical_descriptor = NULL;
	endpoint->tail_physical_descriptor = 0;
	endpoint->next_logical_endpoint = NULL;
	endpoint->next_physical_endpoint = 0;
	endpoint->lock = lock;
	return endpoint;
}


void
OHCI::_FreeEndpoint(ohci_endpoint_descriptor *endpoint)
{
	if (!endpoint)
		return;

	mutex_destroy(endpoint->lock);
	free(endpoint->lock);

	fStack->FreeChunk((void *)endpoint, endpoint->physical_address,
		sizeof(ohci_endpoint_descriptor));
}


status_t
OHCI::_InsertEndpointForPipe(Pipe *pipe)
{
	TRACE("inserting endpoint for device %u endpoint %u\n",
		pipe->DeviceAddress(), pipe->EndpointAddress());

	ohci_endpoint_descriptor *endpoint = _AllocateEndpoint();
	if (!endpoint) {
		TRACE_ERROR("cannot allocate memory for endpoint\n");
		return B_NO_MEMORY;
	}

	uint32 flags = OHCI_ENDPOINT_SKIP;

	// Set up device and endpoint address
	flags |= OHCI_ENDPOINT_SET_DEVICE_ADDRESS(pipe->DeviceAddress())
		| OHCI_ENDPOINT_SET_ENDPOINT_NUMBER(pipe->EndpointAddress());

	// Set the direction
	switch (pipe->Direction()) {
		case Pipe::In:
			flags |= OHCI_ENDPOINT_DIRECTION_IN;
			break;

		case Pipe::Out:
			flags |= OHCI_ENDPOINT_DIRECTION_OUT;
			break;

		case Pipe::Default:
			flags |= OHCI_ENDPOINT_DIRECTION_DESCRIPTOR;
			break;

		default:
			TRACE_ERROR("direction unknown\n");
			_FreeEndpoint(endpoint);
			return B_ERROR;
	}

	// Set up the speed
	switch (pipe->Speed()) {
		case USB_SPEED_LOWSPEED:
			flags |= OHCI_ENDPOINT_LOW_SPEED;
			break;

		case USB_SPEED_FULLSPEED:
			flags |= OHCI_ENDPOINT_FULL_SPEED;
			break;

		default:
			TRACE_ERROR("unacceptable speed\n");
			_FreeEndpoint(endpoint);
			return B_ERROR;
	}

	// Set the maximum packet size
	flags |= OHCI_ENDPOINT_SET_MAX_PACKET_SIZE(pipe->MaxPacketSize());
	endpoint->flags = flags;

	// Add the endpoint to the appropriate list
	uint32 type = pipe->Type();
	ohci_endpoint_descriptor *head = NULL;
	if (type & USB_OBJECT_CONTROL_PIPE)
		head = fDummyControl;
	else if (type & USB_OBJECT_BULK_PIPE)
		head = fDummyBulk;
	else if (type & USB_OBJECT_INTERRUPT_PIPE)
		head = _FindInterruptEndpoint(pipe->Interval());
	else if (type & USB_OBJECT_ISO_PIPE)
		head = fDummyIsochronous;
	else
		TRACE_ERROR("unknown pipe type\n");

	if (head == NULL) {
		TRACE_ERROR("no list found for endpoint\n");
		_FreeEndpoint(endpoint);
		return B_ERROR;
	}

	// Create (necessary) tail descriptor
	if (pipe->Type() & USB_OBJECT_ISO_PIPE) {
		// Set the isochronous bit format
		endpoint->flags |= OHCI_ENDPOINT_ISOCHRONOUS_FORMAT;
		ohci_isochronous_td *tail = _CreateIsochronousDescriptor(0);
		tail->flags = 0;
		endpoint->tail_logical_descriptor = tail;
		endpoint->head_physical_descriptor = tail->physical_address;
		endpoint->tail_physical_descriptor = tail->physical_address;
	} else {
		ohci_general_td *tail = _CreateGeneralDescriptor(0);
		tail->flags = 0;
		endpoint->tail_logical_descriptor = tail;
		endpoint->head_physical_descriptor = tail->physical_address;
		endpoint->tail_physical_descriptor = tail->physical_address;
	}

	if (!_LockEndpoints()) {
		if (endpoint->tail_logical_descriptor) {
			_FreeGeneralDescriptor(
				(ohci_general_td *)endpoint->tail_logical_descriptor);
		}

		_FreeEndpoint(endpoint);
		return B_ERROR;
	}

	pipe->SetControllerCookie((void *)endpoint);
	endpoint->next_logical_endpoint = head->next_logical_endpoint;
	endpoint->next_physical_endpoint = head->next_physical_endpoint;
	head->next_logical_endpoint = (void *)endpoint;
	head->next_physical_endpoint = (uint32)endpoint->physical_address;

	_UnlockEndpoints();
	return B_OK;
}


status_t
OHCI::_RemoveEndpointForPipe(Pipe *pipe)
{
	TRACE("removing endpoint for device %u endpoint %u\n",
		pipe->DeviceAddress(), pipe->EndpointAddress());

	ohci_endpoint_descriptor *endpoint
		= (ohci_endpoint_descriptor *)pipe->ControllerCookie();
	if (endpoint == NULL)
		return B_OK;

	// TODO implement properly, but at least disable it for now
	endpoint->flags |= OHCI_ENDPOINT_SKIP;
	return B_OK;
}


ohci_endpoint_descriptor *
OHCI::_FindInterruptEndpoint(uint8 interval)
{
	uint32 index = 0;
	uint32 power = 1;
	while (power <= OHCI_BIGGEST_INTERVAL / 2) {
		if (power * 2 > interval)
			break;

		power *= 2;
		index++;
	}

	return fInterruptEndpoints[index];
}


ohci_general_td *
OHCI::_CreateGeneralDescriptor(size_t bufferSize)
{
	ohci_general_td *descriptor;
	phys_addr_t physicalAddress;

	if (fStack->AllocateChunk((void **)&descriptor, &physicalAddress,
		sizeof(ohci_general_td)) != B_OK) {
		TRACE_ERROR("failed to allocate general descriptor\n");
		return NULL;
	}

	descriptor->physical_address = (uint32)physicalAddress;
	descriptor->next_physical_descriptor = 0;
	descriptor->next_logical_descriptor = NULL;
	descriptor->buffer_size = bufferSize;
	if (bufferSize == 0) {
		descriptor->buffer_physical = 0;
		descriptor->buffer_logical = NULL;
		descriptor->last_physical_byte_address = 0;
		return descriptor;
	}

	if (fStack->AllocateChunk(&descriptor->buffer_logical,
		&physicalAddress, bufferSize) != B_OK) {
		TRACE_ERROR("failed to allocate space for buffer\n");
		fStack->FreeChunk(descriptor, descriptor->physical_address,
			sizeof(ohci_general_td));
		return NULL;
	}
	descriptor->buffer_physical = physicalAddress;

	descriptor->last_physical_byte_address
		= descriptor->buffer_physical + bufferSize - 1;
	return descriptor;
}


void
OHCI::_FreeGeneralDescriptor(ohci_general_td *descriptor)
{
	if (!descriptor)
		return;

	if (descriptor->buffer_logical) {
		fStack->FreeChunk(descriptor->buffer_logical,
			descriptor->buffer_physical, descriptor->buffer_size);
	}

	fStack->FreeChunk((void *)descriptor, descriptor->physical_address,
		sizeof(ohci_general_td));
}


status_t
OHCI::_CreateDescriptorChain(ohci_general_td **_firstDescriptor,
	ohci_general_td **_lastDescriptor, uint32 direction, size_t bufferSize)
{
	size_t blockSize = 8192;
	int32 descriptorCount = (bufferSize + blockSize - 1) / blockSize;
	if (descriptorCount == 0)
		descriptorCount = 1;

	ohci_general_td *firstDescriptor = NULL;
	ohci_general_td *lastDescriptor = *_firstDescriptor;
	for (int32 i = 0; i < descriptorCount; i++) {
		ohci_general_td *descriptor = _CreateGeneralDescriptor(
			min_c(blockSize, bufferSize));

		if (!descriptor) {
			_FreeDescriptorChain(firstDescriptor);
			return B_NO_MEMORY;
		}

		descriptor->flags = direction
			| OHCI_TD_BUFFER_ROUNDING
			| OHCI_TD_SET_CONDITION_CODE(OHCI_TD_CONDITION_NOT_ACCESSED)
			| OHCI_TD_SET_DELAY_INTERRUPT(OHCI_TD_INTERRUPT_NONE)
			| OHCI_TD_TOGGLE_CARRY;

		// link to previous
		if (lastDescriptor)
			_LinkDescriptors(lastDescriptor, descriptor);

		bufferSize -= blockSize;
		lastDescriptor = descriptor;
		if (!firstDescriptor)
			firstDescriptor = descriptor;
	}

	*_firstDescriptor = firstDescriptor;
	*_lastDescriptor = lastDescriptor;
	return B_OK;
}


void
OHCI::_FreeDescriptorChain(ohci_general_td *topDescriptor)
{
	ohci_general_td *current = topDescriptor;
	ohci_general_td *next = NULL;

	while (current) {
		next = (ohci_general_td *)current->next_logical_descriptor;
		_FreeGeneralDescriptor(current);
		current = next;
	}
}


ohci_isochronous_td *
OHCI::_CreateIsochronousDescriptor(size_t bufferSize)
{
	ohci_isochronous_td *descriptor = NULL;
	phys_addr_t physicalAddress;

	if (fStack->AllocateChunk((void **)&descriptor, &physicalAddress,
		sizeof(ohci_isochronous_td)) != B_OK) {
		TRACE_ERROR("failed to allocate isochronous descriptor\n");
		return NULL;
	}

	descriptor->physical_address = (uint32)physicalAddress;
	descriptor->next_physical_descriptor = 0;
	descriptor->next_logical_descriptor = NULL;
	descriptor->next_done_descriptor = NULL;
	descriptor->buffer_size = bufferSize;
	if (bufferSize == 0) {
		descriptor->buffer_page_byte_0 = 0;
		descriptor->buffer_logical = NULL;
		descriptor->last_byte_address = 0;
		return descriptor;
	}

	if (fStack->AllocateChunk(&descriptor->buffer_logical,
		&physicalAddress, bufferSize) != B_OK) {
		TRACE_ERROR("failed to allocate space for iso.buffer\n");
		fStack->FreeChunk(descriptor, descriptor->physical_address,
			sizeof(ohci_isochronous_td));
		return NULL;
	}
	descriptor->buffer_page_byte_0 = (uint32)physicalAddress;
	descriptor->last_byte_address
		= descriptor->buffer_page_byte_0 + bufferSize - 1;

	return descriptor;
}


void
OHCI::_FreeIsochronousDescriptor(ohci_isochronous_td *descriptor)
{
	if (!descriptor)
		return;

	if (descriptor->buffer_logical) {
		fStack->FreeChunk(descriptor->buffer_logical,
			descriptor->buffer_page_byte_0, descriptor->buffer_size);
	}

	/*	Free the ITD as the size it was ALLOCATED at.

		_CreateIsochronousDescriptor() allocates
		sizeof(ohci_isochronous_td), but stock freed sizeof(ohci_general_td)
		here -- on x86_64, 80 bytes versus 48. The
		USB stack's allocator is a buddy allocator whose bucket is chosen by
		size -- Stack.cpp builds it as PhysicalMemoryAllocator("USB Stack
		Allocator", 8, B_PAGE_SIZE * 32, 64), so buckets are powers of two from
		8 -- which puts the allocation in the 128-byte array and the free in
		the 64-byte array.

		The free is not merely lost. Allocate() marks every sub-block of the
		128-byte slot as used ("fill upwards to the smallest block"), so the
		64-byte entry the free lands on reads as allocated and passes
		Deallocate()'s "address was not allocated!" guard. The result per ITD
		is one 128-byte slot leaked AND one wrongly cleared 64-byte slot, plus
		its parent counters decremented against a block that is still live.

		WHY IT SURFACES LATE. This costs a couple of descriptors per transfer,
		so it needs a stream that actually runs to bite. Before the sizing fix
		in _CreateIsochronousDescriptorChain() OHCI audio died within a second
		or two and never sustained the allocation rate. With that fixed a
		DDJ-SR played cleanly for about 26 s -- roughly 10,000 ITDs across the
		playback and capture streams -- and then the corrupted tree could no
		longer satisfy a ~4 KB ITD buffer:

			usb error ohci 5: failed to allocate space for iso.buffer
			usb error ohci 5: failed to allocate ITD
			usb_audio output queue_isochronous buf:1 ep:0x62 failed: 0xffffffff

		and every transfer after that point time-overran (ITD condition 0x8,
		len 0): exactly one allocation failure, then 42 ITD errors.

		The same file already frees this object at the right size on
		_CreateIsochronousDescriptor()'s own error path, which is what makes
		this a slip rather than a deliberate asymmetry.

		(buffer_page_byte_0 above is the PAGE-MASKED address, not what
		AllocateChunk() returned -- _CreateIsochronousDescriptorChain() clears
		its low 12 bits to build the ITD's page pointer. Harmless only because
		Deallocate() derives the slot from the logical address whenever one is
		given, and it always is here. Left alone deliberately: correcting it
		means storing the unmasked address, which grows the descriptor for no
		behavioural gain.)
	*/
	fStack->FreeChunk((void *)descriptor, descriptor->physical_address,
		sizeof(ohci_isochronous_td));
}


status_t
OHCI::_CreateIsochronousDescriptorChain(ohci_isochronous_td **_firstDescriptor,
	ohci_isochronous_td **_lastDescriptor, Transfer *transfer)
{
	Pipe *pipe = transfer->TransferPipe();
	usb_isochronous_data *isochronousData = transfer->IsochronousData();

	size_t dataLength = transfer->FragmentLength();
	size_t packet_count = isochronousData->packet_count;

	if (packet_count == 0) {
		TRACE_ERROR("isochronous packet_count should not be equal to zero.");
		return B_BAD_VALUE;
	}

	/*	Honour the caller's PER-PACKET lengths.

		STOCK BUG: stock computed one uniform packet size here,

			packetSize = dataLength / packet_count;   // rounded up

		and used it as the stride for every frame's offset. It never read
		isochronousData->packet_descriptors[] on submission -- while
		_FinishIsochronousTransfer() DOES read request_length from those same
		descriptors to work out how much actually moved. So the submission path
		and the completion path disagreed about what each packet was.

		WHY IT DESTROYS AUDIO. 44.1 kHz does not divide into 1 ms frames: a
		stream needs a 44/45 sample cadence, which the caller expresses through
		these per-packet lengths. Flattening that to one rounded-up size
		produces a stride that is not a whole number of samples. Measured on a
		Pioneer DDJ-SR (4 ch x 3 B = 12 B per audio frame, 44.1 kHz, its only
		rate): a 10-packet transfer of 5292 bytes gave packetSize 530, which is
		44.17 samples. Every USB frame then ends mid-sample, channel
		interleaving walks forward, and the result is continuous digital
		distortion. The 8-byte overshoot (10 x 530 vs 5292) is what the
		completion path reports as ITD condition 0x8, DATA_OVERRUN.

		Observed on that device: severe distortion with 22-42 occurrences of
		'ITD error: 0x00000008' per track over OHCI, while the same device on
		UHCI and on xHCI played clean -- OHCI was the only one of the three
		that could not express the cadence.

		THE FIX. An OHCI ITD already supports variable per-frame lengths -- a
		frame's length is the gap between consecutive offsets -- so this needs
		no restructuring into one ITD per frame the way UHCI did. The offsets
		just have to come from a running sum of request_length instead of a
		fixed stride, and each ITD's buffer has to be sized from the frames it
		actually carries.
	*/
	if (isochronousData->packet_descriptors == NULL) {
		TRACE_ERROR("isochronous transfer has no packet descriptors.\n");
		return B_BAD_VALUE;
	}

	// The MaxPacketSize check has to be against the LARGEST packet, not an
	// average -- with a cadence the large frames are the ones that can overrun.
	size_t maxRequested = 0;
	for (size_t i = 0; i < packet_count; i++) {
		size_t len = isochronousData->packet_descriptors[i].request_length;
		if (len > maxRequested)
			maxRequested = len;
	}

	if (maxRequested > pipe->MaxPacketSize()) {
		TRACE_ERROR("isochronous packet of %ld bytes is bigger"
			" than pipe MaxPacketSize %ld.", maxRequested,
			pipe->MaxPacketSize());
		return B_BAD_VALUE;
	}

	uint16 bandwidth = transfer->Bandwidth() / packet_count;
	if (transfer->Bandwidth() % packet_count != 0)
		bandwidth++;

	ohci_isochronous_td *firstDescriptor = NULL;
	ohci_isochronous_td *lastDescriptor = *_firstDescriptor;

	// the frame number currently processed by the host controller
	uint16 currentFrame = fHcca->current_frame_number & 0xFFFF;
	uint16 safeFrames = 5;

	// The entry where to start inserting the first Isochronous descriptor
	// real frame number may differ in case provided one has not bandwidth
	if (isochronousData->flags & USB_ISO_ASAP ||
		isochronousData->starting_frame_number == NULL)
		// We should stay about 5-10 ms ahead of the controller
		// USB1 frame is equal to 1 ms
		currentFrame += safeFrames;
	else
		currentFrame = *isochronousData->starting_frame_number;

	uint16 packets = packet_count;
	uint16 frameOffset = 0;
	// How much of the fragment has been placed into ITD buffers so
	// far, so a caller whose descriptors disagree with dataLength gets clamped
	// rather than overrunning the buffers the copy path fills.
	size_t bytesPlaced = 0;
	while (packets > 0) {
		// look for up to 8 continous frames with available bandwidth
		uint16 frameCount = 0;
		while (frameCount < min_c(OHCI_ITD_NOFFSET, packets)
				&& _AllocateIsochronousBandwidth(frameOffset + currentFrame
					+ frameCount, bandwidth))
			frameCount++;

		if (frameCount == 0) {
			// starting frame has no bandwidth for our transaction - try next
			if (++frameOffset >= 0xFFFF) {
				TRACE_ERROR("failed to allocate bandwidth\n");
				// Hand back what earlier iterations reserved. Nothing
				// was taken this iteration (frameCount is 0), but every
				// descriptor already in the chain still holds its frames.
				_ReleaseIsochronousChainBandwidth(firstDescriptor);
				_FreeIsochronousDescriptorChain(firstDescriptor);
				return B_NO_MEMORY;
			}
			continue;
		}

		// Size this ITD from the frames it actually carries, not from
		// a uniform stride. packetIndex is where these frames sit in the
		// caller's packet_descriptors[].
		size_t packetIndex = packet_count - packets;
		size_t itdLength = 0;
		for (uint16 i = 0; i < frameCount; i++)
			itdLength += isochronousData->packet_descriptors[packetIndex + i]
				.request_length;

		// Never place more than the fragment actually holds. If the caller's
		// descriptors and dataLength ever disagree, clamp rather than run off
		// the end of the buffer the copy path fills.
		if (bytesPlaced + itdLength > dataLength)
			itdLength = dataLength - bytesPlaced;

		ohci_isochronous_td *descriptor
			= _CreateIsochronousDescriptor(itdLength);

		if (!descriptor) {
			TRACE_ERROR("failed to allocate ITD\n");
			// This iteration's reservation is released here...
			_ReleaseIsochronousBandwidth(currentFrame + frameOffset, frameCount);
			// ...and the chain built by earlier iterations is released too.
			_ReleaseIsochronousChainBandwidth(firstDescriptor);
			_FreeIsochronousDescriptorChain(firstDescriptor);
			return B_NO_MEMORY;
		}

		uint16 pageOffset = descriptor->buffer_page_byte_0 & 0xfff;
		descriptor->buffer_page_byte_0 &= ~0xfff;
		// Running sum, not a fixed stride. Frame i starts where frame
		// i-1 ended, so each frame gets exactly its own request_length. This is
		// what carries the 44/45 sample cadence that a uniform stride cannot
		// express -- see the comment on the MaxPacketSize check above.
		size_t frameOffsetBytes = 0;
		for (uint16 i = 0; i < frameCount; i++) {
			descriptor->offset[OHCI_ITD_OFFSET_IDX(i)]
				= OHCI_ITD_MK_OFFS(pageOffset + frameOffsetBytes);
			frameOffsetBytes += isochronousData->packet_descriptors[
				packetIndex + i].request_length;
		}

		descriptor->flags = OHCI_ITD_SET_FRAME_COUNT(frameCount)
				| OHCI_ITD_SET_CONDITION_CODE(OHCI_ITD_CONDITION_NOT_ACCESSED)
				| OHCI_ITD_SET_DELAY_INTERRUPT(OHCI_ITD_INTERRUPT_NONE)
				| OHCI_ITD_SET_STARTING_FRAME(currentFrame + frameOffset);

		// The previous "last packet may be shorter" adjustment is gone.
		//
		//     if (packets <= OHCI_ITD_NOFFSET)
		//         descriptor->last_byte_address
		//             += dataLength - packetSize * (packet_count);
		//
		// It existed only to patch up the uniform-stride assumption after the
		// fact -- and it patched the LAST descriptor by the error accumulated
		// across the WHOLE transfer, which is not where that error lived.
		// _CreateIsochronousDescriptor() already sets last_byte_address from
		// the buffer size, and that size is now exactly the sum of this ITD's
		// own request_lengths, so it is correct with nothing to correct.
		bytesPlaced += itdLength;

		// link to previous
		if (lastDescriptor)
			_LinkIsochronousDescriptors(lastDescriptor, descriptor, descriptor);

		lastDescriptor = descriptor;
		if (!firstDescriptor)
			firstDescriptor = descriptor;

		packets -= frameCount;

		frameOffset += frameCount;

		/*	Report the frame this transfer STARTS in, not the
			frame after it ends.

			STOCK BUG: stock wrote back `currentFrame + frameOffset`. By this
			point frameOffset has already been advanced past every frame the
			transfer occupies, so the value names the first frame AFTER the
			transfer.

			uhci.cpp:1529 (`*starting_frame_number = currentFrame`) and
			xhci.cpp:1231 (`*starting_frame_number = frame`) both report the
			START frame, and usb_audio is written against that contract: after
			a successful full-speed queue it chains the next buffer with

				fNextStartFrame = fStartingFrame + fPacketsPerBuffer;

			(Stream.cpp:1485). Given OHCI's end-frame that addition counts the
			transfer's length twice, so every buffer is scheduled packet_count
			frames later than it should be. At 44.1 kHz (10 packets/buffer)
			that is a 10 ms hole in every 20 ms -- continuous glitching from
			the first buffer -- and the requested frame number gains 10 frames
			per 10 ms buffer against the controller's own counter. Once that
			lead passes half of the 16-bit frame space the ITDs read as
			scheduled in the PAST, and the HC retires them unprocessed: ITD
			condition code 0x8 with every frame left NotAccessed, which
			reaches usb_audio as B_DEV_DATA_OVERRUN with len 0.

			This is why OHCI failed identically before and after the
			packet-sizing fix above, and why UHCI and xHCI are clean with the
			same device, same usb_audio, same format.

			firstDescriptor's own starting frame is by construction the frame
			the transfer begins in -- including any frames the bandwidth
			search had to skip past -- so read it back from there rather than
			recomputing it.
		*/
		if (packets == 0 && isochronousData->starting_frame_number)
			*isochronousData->starting_frame_number
				= OHCI_ITD_GET_STARTING_FRAME(firstDescriptor->flags);
	}

	*_firstDescriptor = firstDescriptor;
	*_lastDescriptor = lastDescriptor;

	return B_OK;
}


/*!	Release the bandwidth held by a whole descriptor chain.

	⚠️ THIS EXISTS BECAUSE FIXING _ReleaseIsochronousBandwidth() UNMASKED A LEAK.

	Both error exits in _CreateIsochronousDescriptorChain() call
	_FreeIsochronousDescriptorChain(), which frees memory and nothing else -- it
	never touched bandwidth. So descriptors created on EARLIER iterations of the
	build loop kept their reservations when a later iteration failed.

	Under stock that was invisible: release reset whole frames to
	MAX_AVAILABLE_BANDWIDTH, so a leak could not accumulate against accounting
	that was already meaningless. Once release became exact, the leak became real
	and permanent -- those frames would stay reduced with nothing left to return
	them, and repeated failures would starve the bus.

	Call this before _FreeIsochronousDescriptorChain() on an ERROR path only.
	⚠️ NOT on the normal completion path: _FinishIsochronousTransfer() already
	releases per descriptor as each ITD completes, and doing both would
	double-release, which this warning exists to prevent rather than trusting
	the reader to notice.
*/
void
OHCI::_ReleaseIsochronousChainBandwidth(ohci_isochronous_td *topDescriptor)
{
	ohci_isochronous_td *current = topDescriptor;

	while (current) {
		_ReleaseIsochronousBandwidth(
			OHCI_ITD_GET_STARTING_FRAME(current->flags),
			OHCI_ITD_GET_FRAME_COUNT(current->flags));
		current = (ohci_isochronous_td *)current->next_done_descriptor;
	}
}


void
OHCI::_FreeIsochronousDescriptorChain(ohci_isochronous_td *topDescriptor)
{
	ohci_isochronous_td *current = topDescriptor;
	ohci_isochronous_td *next = NULL;

	while (current) {
		next = (ohci_isochronous_td *)current->next_done_descriptor;
		_FreeIsochronousDescriptor(current);
		current = next;
	}
}


size_t
OHCI::_WriteDescriptorChain(ohci_general_td *topDescriptor, generic_io_vec *vector,
	size_t vectorCount, bool physical)
{
	ohci_general_td *current = topDescriptor;
	size_t actualLength = 0;
	size_t vectorIndex = 0;
	size_t vectorOffset = 0;
	size_t bufferOffset = 0;

	while (current) {
		if (!current->buffer_logical)
			break;

		while (true) {
			size_t length = min_c(current->buffer_size - bufferOffset,
				vector[vectorIndex].length - vectorOffset);

			TRACE("copying %ld bytes to bufferOffset %ld from"
				" vectorOffset %ld at index %ld of %ld\n", length, bufferOffset,
				vectorOffset, vectorIndex, vectorCount);
			status_t status = generic_memcpy(
				(generic_addr_t)current->buffer_logical + bufferOffset, false,
				vector[vectorIndex].base + vectorOffset, physical, length);
			ASSERT_ALWAYS(status == B_OK);

			actualLength += length;
			vectorOffset += length;
			bufferOffset += length;

			if (vectorOffset >= vector[vectorIndex].length) {
				if (++vectorIndex >= vectorCount) {
					TRACE("wrote descriptor chain (%ld bytes, no"
						" more vectors)\n", actualLength);
					return actualLength;
				}

				vectorOffset = 0;
			}

			if (bufferOffset >= current->buffer_size) {
				bufferOffset = 0;
				break;
			}
		}

		if (!current->next_logical_descriptor)
			break;

		current = (ohci_general_td *)current->next_logical_descriptor;
	}

	TRACE("wrote descriptor chain (%ld bytes)\n", actualLength);
	return actualLength;
}


size_t
OHCI::_WriteIsochronousDescriptorChain(ohci_isochronous_td *topDescriptor,
	generic_io_vec *vector, size_t vectorCount, bool physical)
{
	ohci_isochronous_td *current = topDescriptor;
	size_t actualLength = 0;
	size_t vectorIndex = 0;
	size_t vectorOffset = 0;
	size_t bufferOffset = 0;

	while (current) {
		if (!current->buffer_logical)
			break;

		while (true) {
			size_t length = min_c(current->buffer_size - bufferOffset,
				vector[vectorIndex].length - vectorOffset);

			TRACE("copying %ld bytes to bufferOffset %ld from"
				" vectorOffset %ld at index %ld of %ld\n", length, bufferOffset,
				vectorOffset, vectorIndex, vectorCount);
			status_t status = generic_memcpy(
				(generic_addr_t)current->buffer_logical + bufferOffset, false,
				vector[vectorIndex].base + vectorOffset, physical, length);
			ASSERT_ALWAYS(status == B_OK);

			actualLength += length;
			vectorOffset += length;
			bufferOffset += length;

			if (vectorOffset >= vector[vectorIndex].length) {
				if (++vectorIndex >= vectorCount) {
					TRACE("wrote descriptor chain (%ld bytes, no"
						" more vectors)\n", actualLength);
					return actualLength;
				}

				vectorOffset = 0;
			}

			if (bufferOffset >= current->buffer_size) {
				bufferOffset = 0;
				break;
			}
		}

		if (!current->next_logical_descriptor)
			break;

		current = (ohci_isochronous_td *)current->next_logical_descriptor;
	}

	TRACE("wrote descriptor chain (%ld bytes)\n", actualLength);
	return actualLength;
}


size_t
OHCI::_ReadDescriptorChain(ohci_general_td *topDescriptor, generic_io_vec *vector,
	size_t vectorCount, bool physical)
{
	ohci_general_td *current = topDescriptor;
	size_t actualLength = 0;
	size_t vectorIndex = 0;
	size_t vectorOffset = 0;
	size_t bufferOffset = 0;

	while (current && OHCI_TD_GET_CONDITION_CODE(current->flags)
		!= OHCI_TD_CONDITION_NOT_ACCESSED) {
		if (!current->buffer_logical)
			break;

		size_t bufferSize = current->buffer_size;
		if (current->buffer_physical != 0) {
			bufferSize -= current->last_physical_byte_address
				- current->buffer_physical + 1;
		}

		while (true) {
			size_t length = min_c(bufferSize - bufferOffset,
				vector[vectorIndex].length - vectorOffset);

			TRACE("copying %ld bytes to vectorOffset %ld from"
				" bufferOffset %ld at index %ld of %ld\n", length, vectorOffset,
				bufferOffset, vectorIndex, vectorCount);
			status_t status = generic_memcpy(
				vector[vectorIndex].base + vectorOffset, physical,
				(generic_addr_t)current->buffer_logical + bufferOffset, false, length);
			ASSERT_ALWAYS(status == B_OK);

			actualLength += length;
			vectorOffset += length;
			bufferOffset += length;

			if (vectorOffset >= vector[vectorIndex].length) {
				if (++vectorIndex >= vectorCount) {
					TRACE("read descriptor chain (%ld bytes, no more vectors)\n",
						actualLength);
					return actualLength;
				}

				vectorOffset = 0;
			}

			if (bufferOffset >= bufferSize) {
				bufferOffset = 0;
				break;
			}
		}

		current = (ohci_general_td *)current->next_logical_descriptor;
	}

	TRACE("read descriptor chain (%ld bytes)\n", actualLength);
	return actualLength;
}


void
OHCI::_ReadIsochronousDescriptorChain(ohci_isochronous_td *topDescriptor,
	generic_io_vec *vector, size_t vectorCount, bool physical)
{
	ohci_isochronous_td *current = topDescriptor;
	size_t actualLength = 0;
	size_t vectorIndex = 0;
	size_t vectorOffset = 0;
	size_t bufferOffset = 0;

	while (current && OHCI_ITD_GET_CONDITION_CODE(current->flags)
			!= OHCI_ITD_CONDITION_NOT_ACCESSED) {
		size_t bufferSize = current->buffer_size;
		if (current->buffer_logical != NULL && bufferSize > 0) {
			while (true) {
				size_t length = min_c(bufferSize - bufferOffset,
					vector[vectorIndex].length - vectorOffset);

				TRACE("copying %ld bytes to vectorOffset %ld from bufferOffset"
					" %ld at index %ld of %ld\n", length, vectorOffset,
					bufferOffset, vectorIndex, vectorCount);
				status_t status = generic_memcpy(
					vector[vectorIndex].base + vectorOffset, physical,
					(generic_addr_t)current->buffer_logical + bufferOffset, false, length);
				ASSERT_ALWAYS(status == B_OK);

				actualLength += length;
				vectorOffset += length;
				bufferOffset += length;

				if (vectorOffset >= vector[vectorIndex].length) {
					if (++vectorIndex >= vectorCount) {
						TRACE("read descriptor chain (%ld bytes, "
							"no more vectors)\n", actualLength);
						return;
					}

					vectorOffset = 0;
				}

				if (bufferOffset >= bufferSize) {
					bufferOffset = 0;
					break;
				}
			}
		}

		current = (ohci_isochronous_td *)current->next_done_descriptor;
	}

	TRACE("read descriptor chain (%ld bytes)\n", actualLength);
	return;
}


size_t
OHCI::_ReadActualLength(ohci_general_td *topDescriptor)
{
	ohci_general_td *current = topDescriptor;
	size_t actualLength = 0;

	while (current && OHCI_TD_GET_CONDITION_CODE(current->flags)
		!= OHCI_TD_CONDITION_NOT_ACCESSED) {
		size_t length = current->buffer_size;
		if (current->buffer_physical != 0) {
			length -= current->last_physical_byte_address
				- current->buffer_physical + 1;
		}

		actualLength += length;
		current = (ohci_general_td *)current->next_logical_descriptor;
	}

	TRACE("read actual length (%ld bytes)\n", actualLength);
	return actualLength;
}


void
OHCI::_LinkDescriptors(ohci_general_td *first, ohci_general_td *second)
{
	first->next_physical_descriptor = second->physical_address;
	first->next_logical_descriptor = second;
}


void
OHCI::_LinkIsochronousDescriptors(ohci_isochronous_td *first,
	ohci_isochronous_td *second, ohci_isochronous_td *nextDone)
{
	first->next_physical_descriptor = second->physical_address;
	first->next_logical_descriptor = second;
	first->next_done_descriptor = nextDone;
}


bool
OHCI::_AllocateIsochronousBandwidth(uint16 frame, uint16 size)
{
	frame %= NUMBER_OF_FRAMES;
	if (size > fFrameBandwidth[frame])
		return false;

	fFrameBandwidth[frame]-= size;
	return true;
}


void
OHCI::_ReleaseIsochronousBandwidth(uint16 startFrame, uint16 frameCount)
{
	for (size_t index = 0; index < frameCount; index++) {
		uint16 frame = (startFrame + index) % NUMBER_OF_FRAMES;
		fFrameBandwidth[frame] = MAX_AVAILABLE_BANDWIDTH;
	}
}


status_t
OHCI::_GetStatusOfConditionCode(uint8 conditionCode)
{
	switch (conditionCode) {
		case OHCI_TD_CONDITION_NO_ERROR:
			return B_OK;

		case OHCI_TD_CONDITION_CRC_ERROR:
		case OHCI_TD_CONDITION_BIT_STUFFING:
		case OHCI_TD_CONDITION_TOGGLE_MISMATCH:
			return B_DEV_CRC_ERROR;

		case OHCI_TD_CONDITION_STALL:
			return B_DEV_STALLED;

		case OHCI_TD_CONDITION_NO_RESPONSE:
			return B_TIMED_OUT;

		case OHCI_TD_CONDITION_PID_CHECK_FAILURE:
			return B_DEV_BAD_PID;

		case OHCI_TD_CONDITION_UNEXPECTED_PID:
			return B_DEV_UNEXPECTED_PID;

		case OHCI_TD_CONDITION_DATA_OVERRUN:
			return B_DEV_DATA_OVERRUN;

		case OHCI_TD_CONDITION_DATA_UNDERRUN:
			return B_DEV_DATA_UNDERRUN;

		case OHCI_TD_CONDITION_BUFFER_OVERRUN:
			return B_DEV_WRITE_ERROR;

		case OHCI_TD_CONDITION_BUFFER_UNDERRUN:
			return B_DEV_READ_ERROR;

		case OHCI_TD_CONDITION_NOT_ACCESSED:
			return B_DEV_PENDING;

		case 0x0E:
			return B_DEV_TOO_LATE; // PSW: _NOT_ACCESSED

		default:
			break;
	}

	return B_ERROR;
}


bool
OHCI::_LockEndpoints()
{
	return (mutex_lock(&fEndpointLock) == B_OK);
}


void
OHCI::_UnlockEndpoints()
{
	mutex_unlock(&fEndpointLock);
}


inline void
OHCI::_WriteReg(uint32 reg, uint32 value)
{
	*(volatile uint32 *)(fOperationalRegisters + reg) = value;
}


inline uint32
OHCI::_ReadReg(uint32 reg)
{
	return *(volatile uint32 *)(fOperationalRegisters + reg);
}


void
OHCI::_PrintEndpoint(ohci_endpoint_descriptor *endpoint)
{
	dprintf("endpoint %p\n", endpoint);
	dprintf("\tflags........... 0x%08" B_PRIx32 "\n", endpoint->flags);
	dprintf("\ttail_physical... 0x%08" B_PRIx32 "\n", endpoint->tail_physical_descriptor);
	dprintf("\thead_physical... 0x%08" B_PRIx32 "\n", endpoint->head_physical_descriptor);
	dprintf("\tnext_physical... 0x%08" B_PRIx32 "\n", endpoint->next_physical_endpoint);
	dprintf("\tphysical........ 0x%08" B_PRIx32 "\n", endpoint->physical_address);
	dprintf("\ttail_logical.... %p\n", endpoint->tail_logical_descriptor);
	dprintf("\tnext_logical.... %p\n", endpoint->next_logical_endpoint);
}


void
OHCI::_PrintDescriptorChain(ohci_general_td *topDescriptor)
{
	while (topDescriptor) {
		dprintf("descriptor %p\n", topDescriptor);
		dprintf("\tflags........... 0x%08" B_PRIx32 "\n", topDescriptor->flags);
		dprintf("\tbuffer_physical. 0x%08" B_PRIx32 "\n", topDescriptor->buffer_physical);
		dprintf("\tnext_physical... 0x%08" B_PRIx32 "\n", topDescriptor->next_physical_descriptor);
		dprintf("\tlast_byte....... 0x%08" B_PRIx32 "\n", topDescriptor->last_physical_byte_address);
		dprintf("\tphysical........ 0x%08" B_PRIx32 "\n", topDescriptor->physical_address);
		dprintf("\tbuffer_size..... %lu\n", topDescriptor->buffer_size);
		dprintf("\tbuffer_logical.. %p\n", topDescriptor->buffer_logical);
		dprintf("\tnext_logical.... %p\n", topDescriptor->next_logical_descriptor);

		topDescriptor = (ohci_general_td *)topDescriptor->next_logical_descriptor;
	}
}


void
OHCI::_PrintDescriptorChain(ohci_isochronous_td *topDescriptor)
{
	while (topDescriptor) {
		dprintf("iso.descriptor %p\n", topDescriptor);
		dprintf("\tflags........... 0x%08" B_PRIx32 "\n", topDescriptor->flags);
		dprintf("\tbuffer_pagebyte0 0x%08" B_PRIx32 "\n", topDescriptor->buffer_page_byte_0);
		dprintf("\tnext_physical... 0x%08" B_PRIx32 "\n", topDescriptor->next_physical_descriptor);
		dprintf("\tlast_byte....... 0x%08" B_PRIx32 "\n", topDescriptor->last_byte_address);
		dprintf("\toffset:\n\t0x%04x 0x%04x 0x%04x 0x%04x\n"
							"\t0x%04x 0x%04x 0x%04x 0x%04x\n",
				topDescriptor->offset[0], topDescriptor->offset[1],
				topDescriptor->offset[2], topDescriptor->offset[3],
				topDescriptor->offset[4], topDescriptor->offset[5],
				topDescriptor->offset[6], topDescriptor->offset[7]);
		dprintf("\tphysical........ 0x%08" B_PRIx32 "\n", topDescriptor->physical_address);
		dprintf("\tbuffer_size..... %lu\n", topDescriptor->buffer_size);
		dprintf("\tbuffer_logical.. %p\n", topDescriptor->buffer_logical);
		dprintf("\tnext_logical.... %p\n", topDescriptor->next_logical_descriptor);
		dprintf("\tnext_done....... %p\n", topDescriptor->next_done_descriptor);

		topDescriptor = (ohci_isochronous_td *)topDescriptor->next_done_descriptor;
	}
}

