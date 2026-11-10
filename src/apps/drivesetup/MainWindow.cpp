/*
 * Copyright 2002-2013 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Ithamar R. Adema <ithamar@unet.nl>
 *		Stephan Aßmus <superstippi@gmx.de>
 *		Axel Dörfler, axeld@pinc-software.de.
 *		Erik Jaesler <ejakowatz@users.sourceforge.net>
 *		Ingo Weinhold <ingo_weinhold@gmx.de>
 */


#include "MainWindow.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <Alert.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <ControlLook.h>
#include <Debug.h>
#include <Directory.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceVisitor.h>
#include <DiskDeviceTypes.h>
#include <DiskSystem.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <LayoutBuilder.h>
#include <Locale.h>
#include <MenuItem.h>
#include <MenuBar.h>
#include <Menu.h>
#include <Path.h>
#include <Partition.h>
#include <PartitioningInfo.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <fs_volume.h>
#include <tracker_private.h>

#include "ChangeParametersPanel.h"
#include "ColumnListView.h"
#include "CreateParametersPanel.h"
#include "DiskView.h"
#include "InitParametersPanel.h"
#include "PartitionList.h"
#include "Support.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MainWindow"


enum {
	MSG_MOUNT_ALL				= 'mnta',
	MSG_MOUNT					= 'mnts',
	MSG_UNMOUNT					= 'unmt',
	MSG_FORMAT					= 'frmt',
	MSG_CREATE					= 'crtp',
	MSG_CHANGE					= 'chgp',
	MSG_INITIALIZE				= 'init',
	MSG_DELETE					= 'delt',
	MSG_EJECT					= 'ejct',
	MSG_OPEN_DISKPROBE			= 'opdp',
	MSG_SURFACE_TEST			= 'sfct',
	MSG_RESCAN					= 'rscn',
	MSG_REGISTER				= 'regi',
	MSG_UNREGISTER				= 'unrg',
	MSG_SAVE					= 'save',
	MSG_WRITE					= 'writ',
	MSG_COMPLETE				= 'comp',

	MSG_PARTITION_ROW_SELECTED	= 'prsl',
};


// The name EFI firmware looks for on the removable-media path. Keeping the
// Installer's list here would duplicate it; this is the one DriveSetup needs.
#if defined(__i386__)
static const char* const kEfiLoaderName = "BOOTIA32.EFI";
#elif defined(__x86_64__)
static const char* const kEfiLoaderName = "BOOTX64.EFI";
#elif defined(__arm__)
static const char* const kEfiLoaderName = "BOOTARM.EFI";
#elif defined(__aarch64__)
static const char* const kEfiLoaderName = "BOOTAA64.EFI";
#elif defined(__riscv) && __riscv_xlen == 32
static const char* const kEfiLoaderName = "BOOTRISCV32.EFI";
#elif defined(__riscv) && __riscv_xlen == 64
static const char* const kEfiLoaderName = "BOOTRISCV64.EFI";
#else
static const char* const kEfiLoaderName = NULL;
#endif


/*!	Creates \a directory on the mounted volume if it is not there yet, and
	opens \a name inside it for writing.
*/
static status_t
create_file_at(const BPath& mountPoint, const char* directory,
	const char* name, BFile& file)
{
	BPath path;
	status_t status = path.SetTo(mountPoint.Path(), directory);
	if (status == B_OK)
		status = create_directory(path.Path(), 0755);
	if (status != B_OK)
		return status;

	BDirectory target(path.Path());
	status = target.InitCheck();
	if (status != B_OK)
		return status;

	return target.CreateFile(name, &file, false);
}


static status_t
copy_loader_to(BFile& loader, const BPath& mountPoint, const char* directory,
	const char* name)
{
	BFile file;
	status_t status = create_file_at(mountPoint, directory, name, file);
	if (status != B_OK)
		return status;

	if (loader.Seek(0, SEEK_SET) < 0)
		return B_IO_ERROR;

	// A FAT volume has no attributes to carry over, so a plain copy will do.
	const size_t kChunkSize = 64 * 1024;
	char* buffer = new(std::nothrow) char[kChunkSize];
	if (buffer == NULL)
		return B_NO_MEMORY;

	while (status == B_OK) {
		ssize_t read = loader.Read(buffer, kChunkSize);
		if (read <= 0) {
			status = read < 0 ? (status_t)read : B_OK;
			break;
		}

		ssize_t written = file.Write(buffer, read);
		if (written != read)
			status = written < 0 ? (status_t)written : B_IO_ERROR;
	}

	delete[] buffer;
	return status;
}


class ListPopulatorVisitor : public BDiskDeviceVisitor {
public:
	ListPopulatorVisitor(PartitionListView* list, int32& diskCount,
			SpaceIDMap& spaceIDMap)
		:
		fPartitionList(list),
		fDiskCount(diskCount),
		fSpaceIDMap(spaceIDMap)
	{
		fDiskCount = 0;
		fSpaceIDMap.Clear();
		// start with an empty list
		int32 rows = fPartitionList->CountRows();
		for (int32 i = rows - 1; i >= 0; i--) {
			BRow* row = fPartitionList->RowAt(i);
			fPartitionList->RemoveRow(row);
			delete row;
		}
	}

	virtual bool Visit(BDiskDevice* device)
	{
		fDiskCount++;
		// if we don't prepare the device for modifications,
		// we cannot get information about available empty
		// regions on the device or child partitions
		device->PrepareModifications();
		_AddPartition(device);
		return false; // Don't stop yet!
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		_AddPartition(partition);
		return false; // Don't stop yet!
	}

private:
	void _AddPartition(BPartition* partition) const
	{
		// add the partition itself
		fPartitionList->AddPartition(partition);

		// add any available space on it
		BPartitioningInfo info;
		status_t status = partition->GetPartitioningInfo(&info);
		if (status >= B_OK) {
			partition_id parentID = partition->ID();
			off_t offset;
			off_t size;
			for (int32 i = 0;
					info.GetPartitionableSpaceAt(i, &offset, &size) >= B_OK;
					i++) {
				// TODO: remove again once Disk Device API is fixed
				if (!is_valid_partitionable_space(size))
					continue;
				//
				partition_id id = fSpaceIDMap.SpaceIDFor(parentID, offset);
				fPartitionList->AddSpace(parentID, id, offset, size);
			}
		}
	}

	PartitionListView*	fPartitionList;
	int32&				fDiskCount;
	SpaceIDMap&			fSpaceIDMap;
	BDiskDevice*		fLastPreparedDevice;
};


class MountAllVisitor : public BDiskDeviceVisitor {
public:
	MountAllVisitor()
	{
	}

	virtual bool Visit(BDiskDevice* device)
	{
		if (device->ContainsFileSystem())
			device->Mount();

		return false; // Don't stop yet!
	}

	virtual bool Visit(BPartition* partition, int32 level)
	{
		partition->Mount();
		return false; // Don't stop yet!
	}

private:
	PartitionListView* fPartitionList;
};


class ModificationPreparer {
public:
	ModificationPreparer(BDiskDevice* disk)
		:
		fDisk(disk),
		fModificationStatus(fDisk->PrepareModifications())
	{
	}
	~ModificationPreparer()
	{
		if (fModificationStatus == B_OK)
			fDisk->CancelModifications();
	}
	status_t ModificationStatus() const
	{
		return fModificationStatus;
	}
	status_t CommitModifications()
	{
		status_t status = fDisk->CommitModifications();
		if (status == B_OK)
			fModificationStatus = B_ERROR;

		return status;
	}

private:
	BDiskDevice*	fDisk;
	status_t		fModificationStatus;
};


// #pragma mark -


MainWindow::MainWindow()
	:
	BWindow(BRect(50, 50, 600, 500), B_TRANSLATE_SYSTEM_NAME("DriveSetup"),
		B_DOCUMENT_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fCurrentDisk(NULL),
	fCurrentPartitionID(-1),
	fSpaceIDMap()
{
	fMenuBar = new BMenuBar("root menu");

	// create all the menu items
	fWipeMenuItem = new BMenuItem(B_TRANSLATE("Wipe (not implemented)"),
		new BMessage(MSG_FORMAT));
	fEjectMenuItem = new BMenuItem(B_TRANSLATE("Eject"),
		new BMessage(MSG_EJECT), 'E');
	fOpenDiskProbeMenuItem = new BMenuItem(B_TRANSLATE("Open with DiskProbe"),
		new BMessage(MSG_OPEN_DISKPROBE));

	fSurfaceTestMenuItem = new BMenuItem(
		B_TRANSLATE("Surface test (not implemented)"),
		new BMessage(MSG_SURFACE_TEST));
	fRescanMenuItem = new BMenuItem(B_TRANSLATE("Rescan"),
		new BMessage(MSG_RESCAN));

	fCreateMenuItem = new BMenuItem(B_TRANSLATE("Create" B_UTF8_ELLIPSIS),
		new BMessage(MSG_CREATE), 'C');
	fChangeMenuItem = new BMenuItem(
		B_TRANSLATE("Change parameters" B_UTF8_ELLIPSIS),
		new BMessage(MSG_CHANGE));
	fDeleteMenuItem = new BMenuItem(B_TRANSLATE("Delete"),
		new BMessage(MSG_DELETE), 'D');

	fMountMenuItem = new BMenuItem(B_TRANSLATE("Mount"),
		new BMessage(MSG_MOUNT), 'M');
	fUnmountMenuItem = new BMenuItem(B_TRANSLATE("Unmount"),
		new BMessage(MSG_UNMOUNT), 'U');
	fMountAllMenuItem = new BMenuItem(B_TRANSLATE("Mount all"),
		new BMessage(MSG_MOUNT_ALL), 'M', B_SHIFT_KEY);

	fRegisterMenuItem = new BMenuItem(
		B_TRANSLATE("Register disk image" B_UTF8_ELLIPSIS),
		new BMessage(MSG_REGISTER));
	fUnRegisterMenuItem = new BMenuItem(
		B_TRANSLATE("Unregister disk image"),
		new BMessage(MSG_UNREGISTER));
	fSaveMenuItem = new BMenuItem(
		B_TRANSLATE("Save image" B_UTF8_ELLIPSIS),
		new BMessage(MSG_SAVE));
	fWriteMenuItem = new BMenuItem(
		B_TRANSLATE("Write image" B_UTF8_ELLIPSIS),
		new BMessage(MSG_WRITE));

	// Disk menu
	fDiskMenu = new BMenu(B_TRANSLATE("Disk"));

	// fDiskMenu->AddItem(fWipeMenuItem);
	fDiskInitMenu = new BMenu(B_TRANSLATE("Initialize"));
	fDiskMenu->AddItem(fDiskInitMenu);

	fDiskMenu->AddSeparatorItem();

	fDiskMenu->AddItem(fEjectMenuItem);
	// fDiskMenu->AddItem(fSurfaceTestMenuItem);
	fDiskMenu->AddItem(fRescanMenuItem);

	fMenuBar->AddItem(fDiskMenu);

	// Parition menu
	fPartitionMenu = new BMenu(B_TRANSLATE("Partition"));
	fPartitionMenu->AddItem(fCreateMenuItem);

	fFormatMenu = new BMenu(B_TRANSLATE("Format"));
	fPartitionMenu->AddItem(fFormatMenu);

	fPartitionMenu->AddItem(fChangeMenuItem);
	fPartitionMenu->AddItem(fDeleteMenuItem);

	fPartitionMenu->AddSeparatorItem();

	fPartitionMenu->AddItem(fMountMenuItem);
	fPartitionMenu->AddItem(fUnmountMenuItem);

	fPartitionMenu->AddSeparatorItem();

	fPartitionMenu->AddItem(fMountAllMenuItem);

	fPartitionMenu->AddSeparatorItem();

	fPartitionMenu->AddItem(fOpenDiskProbeMenuItem);
	fMenuBar->AddItem(fPartitionMenu);

	// Disk image menu
	fDiskImageMenu = new BMenu(B_TRANSLATE("Disk image"));

	fDiskImageMenu->AddItem(fRegisterMenuItem);
	fDiskImageMenu->AddItem(fUnRegisterMenuItem);

	fDiskImageMenu->AddSeparatorItem();

	fDiskImageMenu->AddItem(fSaveMenuItem);
	fDiskImageMenu->AddItem(fWriteMenuItem);

	fMenuBar->AddItem(fDiskImageMenu);

	BGroupLayout* layout = new BGroupLayout(B_VERTICAL, 0);
	layout->SetInsets(-1, 0, -1, -1);
	SetLayout(layout);
	AddChild(fMenuBar);

	// Partition / Drives context menu
	fContextMenu = new BPopUpMenu("Partition", false, false);
	fCreateContextMenuItem = new BMenuItem(B_TRANSLATE("Create" B_UTF8_ELLIPSIS),
		new BMessage(MSG_CREATE), 'C');
	fChangeContextMenuItem = new BMenuItem(
		B_TRANSLATE("Change parameters" B_UTF8_ELLIPSIS),
		new BMessage(MSG_CHANGE));
	fDeleteContextMenuItem = new BMenuItem(B_TRANSLATE("Delete"),
		new BMessage(MSG_DELETE), 'D');
	fMountContextMenuItem = new BMenuItem(B_TRANSLATE("Mount"),
		new BMessage(MSG_MOUNT), 'M');
	fUnmountContextMenuItem = new BMenuItem(B_TRANSLATE("Unmount"),
		new BMessage(MSG_UNMOUNT), 'U');
	fOpenDiskProbeContextMenuItem = new BMenuItem(B_TRANSLATE("Open with DiskProbe"),
		new BMessage(MSG_OPEN_DISKPROBE));
	fFormatContextMenuItem = new BMenu(B_TRANSLATE("Format"));

	fContextMenu->AddItem(fCreateContextMenuItem);
	fContextMenu->AddItem(fFormatContextMenuItem);
	fContextMenu->AddItem(fChangeContextMenuItem);
	fContextMenu->AddItem(fDeleteContextMenuItem);
	fContextMenu->AddSeparatorItem();
	fContextMenu->AddItem(fMountContextMenuItem);
	fContextMenu->AddItem(fUnmountContextMenuItem);
	fContextMenu->AddSeparatorItem();
	fContextMenu->AddItem(fOpenDiskProbeContextMenuItem);
	fContextMenu->SetTargetForItems(this);

	// add DiskView
	fDiskView = new DiskView(fSpaceIDMap);
	AddChild(fDiskView);

	// add PartitionListView
	fListView = new PartitionListView();
	AddChild(fListView);

	// configure PartitionListView
	fListView->SetSelectionMode(B_SINGLE_SELECTION_LIST);
	fListView->SetSelectionMessage(new BMessage(MSG_PARTITION_ROW_SELECTED));
	fListView->SetTarget(this);
	fListView->MakeFocus(true);

	fRegisterFilePanel = NULL;
	fWriteImageFilePanel = NULL;
	fReadImageFilePanel = NULL;

	fNotification = new BNotification(B_INFORMATION_NOTIFICATION);
	fNotification->SetGroup(BString(B_TRANSLATE_SYSTEM_NAME("DriveSetup")));

	app_info info;
	be_roster->GetAppInfo("application/x-vnd.Haiku-DriveSetup", &info);
	BBitmap icon(BRect(0, 0, 32, 32), B_RGBA32);
	BNode node(&info.ref);
	if (BIconUtils::GetVectorIcon(&node, "BEOS:ICON", &icon) == B_OK)
		fNotification->SetIcon(&icon);

	status_t status = fDiskDeviceRoster.StartWatching(BMessenger(this));
	if (status != B_OK) {
		fprintf(stderr, "Failed to start watching for device changes: %s\n",
			strerror(status));
	}

	// visit all disks in the system and show their contents
	_ScanDrives();

	// If DeskBar isn't running, DriveSetup was probably started from Installer.
	// Make sure to show on all workspaces, so the window doesn't disappear when using the
	// workspace switch shortcut.
	// Also make it not minimizable, because without DeskBar, there is no way to restore a
	// minimized window.
	if (!be_roster->IsRunning(kDeskbarSignature)) {
		SetFlags(Flags() | B_NOT_MINIMIZABLE);
		SetWorkspaces(B_ALL_WORKSPACES);
	}
}


MainWindow::~MainWindow()
{
	BDiskDeviceRoster().StopWatching(this);
	delete fCurrentDisk;
	delete fContextMenu;

	delete fRegisterFilePanel;
	delete fReadImageFilePanel;
	delete fWriteImageFilePanel;

	delete fNotification;
}


void
MainWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_MOUNT_ALL:
			_MountAll();
			break;
		case MSG_MOUNT:
			_Mount(fCurrentDisk, fCurrentPartitionID);
			break;
		case MSG_UNMOUNT:
			_Unmount(fCurrentDisk, fCurrentPartitionID);
			break;

		case MSG_FORMAT:
			printf("MSG_FORMAT\n");
			break;

		case MSG_CREATE:
			_Create(fCurrentDisk, fCurrentPartitionID);
			break;

		case MSG_INITIALIZE: {
			BString diskSystemName;
			if (message->FindString("disk system", &diskSystemName) != B_OK)
				break;
			_Initialize(fCurrentDisk, fCurrentPartitionID, diskSystemName);
			break;
		}

		case MSG_CHANGE:
			_ChangeParameters(fCurrentDisk, fCurrentPartitionID);
			break;

		case MSG_DELETE:
			_Delete(fCurrentDisk, fCurrentPartitionID);
			break;

		case MSG_EJECT:
			// TODO: completely untested, especially interesting
			// if partition list behaves when partitions disappear
			if (fCurrentDisk) {
				// TODO: only if no partitions are mounted anymore?
				fCurrentDisk->Eject(true);
				_ScanDrives();
			}
			break;
		case MSG_OPEN_DISKPROBE:
		{
			PartitionListRow* row = dynamic_cast<PartitionListRow*>(
				fListView->CurrentSelection());
			const char* args[] = { row->DevicePath(), NULL };

			be_roster->Launch("application/x-vnd.Haiku-DiskProbe", 1,
				(char**)args);

			break;
		}
		case MSG_SURFACE_TEST:
			printf("MSG_SURFACE_TEST\n");
			break;

		// TODO: this could probably be done better!
		case B_DEVICE_UPDATE:
			printf("B_DEVICE_UPDATE\n");
		case MSG_RESCAN:
			_ScanDrives();
			break;

		case MSG_PARTITION_ROW_SELECTED: {
			// selection of partitions via list view
			_AdaptToSelectedPartition();

			BPoint where;
			uint32 buttons;
			fListView->GetMouse(&where, &buttons);
			where.x += 2; // to prevent occasional select
			if (buttons & B_SECONDARY_MOUSE_BUTTON)
				fContextMenu->Go(fListView->ConvertToScreen(where),
					true, false, true);
			break;
		}
		case MSG_SELECTED_PARTITION_ID: {
			// selection of partitions via disk view
			partition_id id;
			if (message->FindInt32("partition_id", &id) == B_OK) {
				if (BRow* row = fListView->FindRow(id)) {
					fListView->DeselectAll();
					fListView->AddToSelection(row);
					_AdaptToSelectedPartition();
				}
			}
			BPoint where;
			uint32 buttons;
			fListView->GetMouse(&where, &buttons);
			where.x += 2; // to prevent occasional select
			if (buttons & B_SECONDARY_MOUSE_BUTTON)
				fContextMenu->Go(fListView->ConvertToScreen(where),
					true, false, true);
			break;
		}

		case MSG_REGISTER:
		{
			entry_ref entryRef;
			message->FindRef("refs", &entryRef);
			BEntry entry(&entryRef);
			BPath path;
			if (entry.GetPath(&path) == B_OK) {
				RegisterFileDiskDevice(path.Path());
				break;
			}

			if (fRegisterFilePanel == NULL) {
				fRegisterFilePanel = new(std::nothrow) BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), NULL, B_FILE_NODE, false,
					new BMessage(MSG_REGISTER), NULL, true);
				BString title(B_TRANSLATE("%appname%: Register disk image"));
				title.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("DriveSetup"));
				fRegisterFilePanel->Window()->SetTitle(title);
				fRegisterFilePanel->SetButtonLabel(B_DEFAULT_BUTTON, B_TRANSLATE("Register"));
			}

			if (fRegisterFilePanel != NULL)
				fRegisterFilePanel->Show();
			break;
		}
		case MSG_UNREGISTER:
		{
			BPath path;
			fCurrentDisk->GetPath(&path);
			UnRegisterFileDiskDevice(path.Path());
			break;
		}
		case MSG_WRITE:
		{
			entry_ref entryRef;
			message->FindRef("refs", &entryRef);
			BEntry entry(&entryRef);
			BPath path;
			if (entry.GetPath(&path) == B_OK) {
				BMessage* msg = new BMessage();

				PartitionListRow* row = dynamic_cast<PartitionListRow*>(
					fListView->CurrentSelection());

				msg->AddString("source", path.Path());
				msg->AddString("target", row->DevicePath());
				msg->AddMessenger("messenger", BMessenger(this));

				thread_id write_thread = spawn_thread(WriteDiskImage,
					"WriteDiskImage", B_LOW_PRIORITY, (void*)msg);
				resume_thread(write_thread);
			} else {
				if (fWriteImageFilePanel == NULL) {
					fWriteImageFilePanel = new(std::nothrow) BFilePanel(
						B_OPEN_PANEL, new BMessenger(this), NULL, B_FILE_NODE,
						false, new BMessage(MSG_WRITE), NULL, true);
					BString title(B_TRANSLATE("%appname%: Select disk image"));
					title.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("DriveSetup"));
					fWriteImageFilePanel->Window()->SetTitle(title);
					fWriteImageFilePanel->SetButtonLabel(B_DEFAULT_BUTTON, B_TRANSLATE("Write"));
				}
				if (fWriteImageFilePanel != NULL)
					fWriteImageFilePanel->Show();
			}
			break;
		}
		case MSG_SAVE:
		{
			PartitionListRow* row = dynamic_cast<PartitionListRow*>(
				fListView->CurrentSelection());

			if (fReadImageFilePanel == NULL) {
				fReadImageFilePanel = new(std::nothrow) BFilePanel(B_SAVE_PANEL,
					new BMessenger(this), NULL, B_FILE_NODE, false, NULL, NULL,
					true);
				BString title(B_TRANSLATE("%appname%: Save disk image"));
				title.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("DriveSetup"));
				fReadImageFilePanel->Window()->SetTitle(title);
			}

			if (fReadImageFilePanel != NULL) {
				BStringField* field = (BStringField*)row->GetField(2);
				fReadImageFilePanel->SetSaveText(field->String());
				fReadImageFilePanel->Show();
			}
			break;
		}
		case B_SAVE_REQUESTED:
		{
			BMessage* msg = new BMessage();

			entry_ref entryRef;
			message->FindRef("directory", &entryRef);
			BEntry entry(&entryRef);
			BPath path;
			entry.GetPath(&path);
			msg->AddString("targetfolder", path.Path());
			const char* name;
			message->FindString("name", &name);
			path.Append(name);

			PartitionListRow* row = dynamic_cast<PartitionListRow*>(
				fListView->CurrentSelection());

			msg->AddString("source", row->DevicePath());
			msg->AddString("target", path.Path());
			msg->AddInt32("partitionID", row->ID());
			msg->AddMessenger("messenger", BMessenger(this));

			thread_id save_thread = spawn_thread(SaveDiskImage, "SaveDiskImage",
				B_LOW_PRIORITY, (void*)msg);
			resume_thread(save_thread);

			break;
		}
		case MSG_COMPLETE:
		{
			const char* status = "";
			const char* jobid = "";

			message->FindString("status", &status);
			message->FindString("jobid", &jobid);

			fNotification->SetTitle(BString(B_TRANSLATE("Disk image")));
			fNotification->SetContent(BString(status));
			fNotification->SetMessageID(BString(jobid));
			fNotification->Send();

			break;
		}

		case MSG_UPDATE_ZOOM_LIMITS:
			_UpdateWindowZoomLimits();
			break;

		case B_REFS_RECEIVED:
		{
			entry_ref ref;
			int32 i = 0;

			while (message->FindRef("refs", i++, &ref) == B_OK) {
				BEntry entry(&ref, true);
				BPath path(&entry);
				BNode node(&entry);

				if (node.IsFile()) {
					if (entry.GetPath(&path) == B_OK)
						RegisterFileDiskDevice(path.Path());
				}
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
MainWindow::QuitRequested()
{
	// TODO: ask about any unsaved changes
	be_app->PostMessage(B_QUIT_REQUESTED);
	Hide();
	return false;
}


// #pragma mark -


status_t
MainWindow::StoreSettings(BMessage* archive) const
{
	if (archive->ReplaceRect("window frame", Frame()) < B_OK)
		archive->AddRect("window frame", Frame());

	BMessage columnSettings;
	fListView->SaveState(&columnSettings);
	if (archive->ReplaceMessage("column settings", &columnSettings) < B_OK)
		archive->AddMessage("column settings", &columnSettings);

	return B_OK;
}


status_t
MainWindow::RestoreSettings(BMessage* archive)
{
	BRect frame;
	if (archive->FindRect("window frame", &frame) == B_OK) {
		BScreen screen(this);
		if (frame.Intersects(screen.Frame())) {
			MoveTo(frame.LeftTop());
			ResizeTo(frame.Width(), frame.Height());
		}
	}

	BMessage columnSettings;
	if (archive->FindMessage("column settings", &columnSettings) == B_OK)
		fListView->LoadState(&columnSettings);

	return B_OK;
}


void
MainWindow::ApplyDefaultSettings()
{
	if (!Lock())
		return;

	fListView->ResizeAllColumnsToPreferred();

	// Adjust window size for convenience
	BScreen screen(this);
	float windowWidth = Frame().Width();
	float windowHeight = Frame().Height();

	float enlargeWidthBy = fListView->PreferredSize().width
		- fListView->Bounds().Width();
	float enlargeHeightBy = fListView->PreferredSize().height
		- fListView->Bounds().Height();

	if (enlargeWidthBy > 0.0f)
		windowWidth += enlargeWidthBy;
	if (enlargeHeightBy > 0.0f)
		windowHeight += enlargeHeightBy;

	if (windowWidth > screen.Frame().Width() - 20.0f)
		windowWidth = screen.Frame().Width() - 20.0f;
	if (windowHeight > screen.Frame().Height() - 20.0f)
		windowHeight = screen.Frame().Height() - 20.0f;

	ResizeTo(windowWidth, windowHeight);
	CenterOnScreen();

	Unlock();
}


static void
FileErrorAlert(const char* text, const char* fileName, alert_type type)
{
	BString message;
	message.SetToFormat(text, fileName);
	BString title(B_TRANSLATE("%appname% error"));
	title.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("DriveSetup"));
	BAlert* alert = new BAlert(title, message.String(),
		B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_FROM_WIDEST, type);
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


status_t
MainWindow::RegisterFileDiskDevice(const char* fileName)
{
	BString message;

	struct stat st;
	if (lstat(fileName, &st) != 0) {
		status_t error = errno;

		FileErrorAlert(B_TRANSLATE("Failed to get information about '%s'."),
			fileName, B_STOP_ALERT);
		return error;
	}

	if (!S_ISREG(st.st_mode)) {
		FileErrorAlert(B_TRANSLATE("'%s' is not a regular file."), fileName,
			B_STOP_ALERT);
		return B_BAD_VALUE;
	}

	// register the file
	BDiskDeviceRoster roster;
	partition_id id = roster.RegisterFileDevice(fileName);
	if (id < 0) {
		FileErrorAlert(B_TRANSLATE("Failed to register file disk device: %s"),
			fileName, B_STOP_ALERT);
		return id;
	}

	fNotification->SetTitle(BString(B_TRANSLATE("Disk image")));
	fNotification->SetContent(BString(
		B_TRANSLATE("Disk image has been added.")));
	fNotification->Send();

	return B_OK;
}


status_t
MainWindow::UnRegisterFileDiskDevice(const char* fileNameOrID)
{
	BString message;

	// try to parse the parameter as ID
	char* numberEnd;
	partition_id id = strtol(fileNameOrID, &numberEnd, 0);
	BDiskDeviceRoster roster;
	if (id >= 0 && numberEnd != fileNameOrID && *numberEnd == '\0') {
		BDiskDevice device;
		if (roster.GetDeviceWithID(id, &device) == B_OK && device.IsFile()) {
			status_t error = roster.UnregisterFileDevice(id);
			if (error != B_OK) {
				FileErrorAlert(B_TRANSLATE(
					"Failed to unregister file disk device."), NULL,
					B_STOP_ALERT);
				return error;
			}

			fNotification->SetTitle(BString(B_TRANSLATE("Disk image")));
			fNotification->SetContent(BString(
				B_TRANSLATE("Disk image has been removed.")));
			fNotification->Send();

			return B_OK;
		} else {
			FileErrorAlert(B_TRANSLATE(
				"No file disk device found with the given ID, trying file '%s'."),
				fileNameOrID, B_WARNING_ALERT);
		}
	}

	// the parameter must be a file name -- stat() it
	struct stat st;
	if (lstat(fileNameOrID, &st) != 0) {
		status_t error = errno;
		FileErrorAlert(B_TRANSLATE("Failed to get information about '%s'."),
			fileNameOrID, B_STOP_ALERT);
		return error;
	}

	// remember the volume and node ID, so we can identify the file
	// NOTE: There's a race condition -- we would need to open the file and
	// keep it open to avoid it.
	dev_t volumeID = st.st_dev;
	ino_t nodeID = st.st_ino;

	// iterate through all file disk devices and try to find a match
	BDiskDevice device;
	while (roster.GetNextDevice(&device) == B_OK) {
		if (!device.IsFile())
			continue;

		// get file path and stat it, same for the device path
		BPath path;
		if ((device.GetFilePath(&path) == B_OK && lstat(path.Path(), &st) == 0
				&& volumeID == st.st_dev && nodeID == st.st_ino)
			|| (device.GetPath(&path) == B_OK && lstat(path.Path(), &st) == 0
				&& volumeID == st.st_dev && nodeID == st.st_ino)) {
			status_t error = roster.UnregisterFileDevice(device.ID());
			if (error != B_OK) {
				FileErrorAlert(B_TRANSLATE(
					"Failed to unregister file disk device %s."), fileNameOrID,
					B_STOP_ALERT);
				return error;
			}

			fNotification->SetTitle(BString(B_TRANSLATE("Disk image")));
			fNotification->SetContent(BString(
				B_TRANSLATE("Disk image has been removed.")));
			fNotification->Send();

			return B_OK;
		}
	}

	FileErrorAlert(B_TRANSLATE("%s does not refer to a file disk device."),
		fileNameOrID, B_WARNING_ALERT);
	return B_BAD_VALUE;
}


int32
MainWindow::SaveDiskImage(void* data)
{
	BMessage* msg = (BMessage*)data;
	const char* sourcepath;
	const char* targetpath;
	const char* targetfolder;
	int32 partitionID;
	BMessenger messenger;

	msg->FindString("source", &sourcepath);
	msg->FindString("target", &targetpath);
	msg->FindString("targetfolder", &targetfolder);
	msg->FindInt32("partitionID", &partitionID);
	msg->FindMessenger("messenger", &messenger);

	BFile source(sourcepath, B_READ_ONLY);
	BFile target(targetpath, B_READ_WRITE | B_CREATE_FILE);

	if (source.InitCheck() != B_OK) {
		FileErrorAlert(B_TRANSLATE("Cannot init source volume."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	if (target.InitCheck() != B_OK) {
		FileErrorAlert(B_TRANSLATE("Cannot init target file."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	BDirectory* targetdir = new BDirectory(targetfolder);
	node_ref node;
	targetdir->GetNodeRef(&node);
	BVolume volume(node.device);

	if (volume.InitCheck()) {
		FileErrorAlert(B_TRANSLATE("Cannot init target volume."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	if (volume.IsReadOnly()) {
		FileErrorAlert(B_TRANSLATE("Target volume is read only."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	BPartition *partition;
	BDiskDevice device;

	BDiskDeviceRoster().GetPartitionWithID(partitionID, &device, &partition);

	if (volume.FreeBytes() < partition->Size()) {
		FileErrorAlert(B_TRANSLATE("There is not enough free space on target "
			"device."), NULL, B_STOP_ALERT);
		return -1;
	}

	_WriteDiskImage(messenger, source, target, targetpath,
		BString(B_TRANSLATE("Creating disk image")),
		BString(B_TRANSLATE("Disk image successfully created.")));

	return 0;
}


int32
MainWindow::WriteDiskImage(void* data)
{
	BMessage* msg = (BMessage*)data;
	const char* sourcepath;
	const char* targetpath;
	BMessenger messenger;

	msg->FindString("source", &sourcepath);
	msg->FindString("target", &targetpath);
	msg->FindMessenger("messenger", &messenger);

	BFile source(sourcepath, B_READ_ONLY);
	BFile target(targetpath, B_READ_WRITE);

	if (target.InitCheck() != B_OK) {
		FileErrorAlert(B_TRANSLATE("Cannot init target partition."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	BPartition *partition;
	BDiskDevice device;

	BDiskDeviceRoster().GetPartitionForPath(targetpath, &device, &partition);

	if (partition->IsReadOnly()) {
		FileErrorAlert(B_TRANSLATE("Target partition is read only."), NULL,
			B_STOP_ALERT);
		return -1;
	}

	off_t size;
	source.GetSize(&size);

	if (partition->Size() < size) {
		FileErrorAlert(B_TRANSLATE("The target partition is smaller than the "
			"image file."), NULL, B_STOP_ALERT);
		return -1;
	}

	_WriteDiskImage(messenger, source, target, targetpath,
		BString(B_TRANSLATE("Writing image to disk")),
		BString(B_TRANSLATE("Disk image successfully written to the target.")));

	return 0;
}


void
MainWindow::_WriteDiskImage(BMessenger messenger, BFile source, BFile target,
	const char* targetpath, BString title, BString status)
{
	size_t bufsize = 1024 * 1024;
	off_t sourcesize;
	ssize_t targetbytes = 0;

	source.GetSize(&sourcesize);

	char maximumBuffer[16], currentBuffer[16];
	BString statusprogress;

	BNotification progress(B_PROGRESS_NOTIFICATION);
	progress.SetGroup(BString(B_TRANSLATE_SYSTEM_NAME("DriveSetup")));
	progress.SetMessageID(BString(targetpath));
	progress.SetTitle(title);

	char* buffer = (char*)malloc(bufsize);
	while (true) {
		ssize_t bytes = source.Read(buffer, bufsize);

		if (bytes > 0) {
			target.Write(buffer, (size_t)bytes);

			targetbytes += bytes;

			statusprogress.SetToFormat("%s: %s / %s", targetpath,
				string_for_size(targetbytes, currentBuffer, 16),
				string_for_size(sourcesize, maximumBuffer, 16));

			progress.SetContent(statusprogress);
			progress.SetProgress((float)targetbytes / sourcesize);
			progress.Send();
		} else
			break;
	}
	free(buffer);

	BMessage message(MSG_COMPLETE);
	message.AddString("status", status);
	message.AddString("jobid", targetpath);
	messenger.SendMessage(&message);
}


// #pragma mark -


void
MainWindow::_ScanDrives()
{
	fSpaceIDMap.Clear();
	int32 diskCount = 0;
	ListPopulatorVisitor driveVisitor(fListView, diskCount, fSpaceIDMap);
	fDiskDeviceRoster.VisitEachPartition(&driveVisitor);
	fDiskView->SetDiskCount(diskCount);

	// restore selection
	PartitionListRow* previousSelection
		= fListView->FindRow(fCurrentPartitionID);
	if (previousSelection) {
		fListView->SetFocusRow(previousSelection, true);
		_UpdateMenus(fCurrentDisk, fCurrentPartitionID,
			previousSelection->ParentID());
		fDiskView->ForceUpdate();
	} else {
		_UpdateMenus(NULL, -1, -1);
	}

	PostMessage(MSG_UPDATE_ZOOM_LIMITS);
}


// #pragma mark -


void
MainWindow::_AdaptToSelectedPartition()
{
	partition_id diskID = -1;
	partition_id partitionID = -1;
	partition_id parentID = -1;

	BRow* _selectedRow = fListView->CurrentSelection();
	if (_selectedRow) {
		// go up to top level row
		BRow* _topLevelRow = _selectedRow;
		BRow* parent = NULL;
		while (fListView->FindParent(_topLevelRow, &parent, NULL))
			_topLevelRow = parent;

		PartitionListRow* topLevelRow
			= dynamic_cast<PartitionListRow*>(_topLevelRow);
		PartitionListRow* selectedRow
			= dynamic_cast<PartitionListRow*>(_selectedRow);

		if (topLevelRow)
			diskID = topLevelRow->ID();
		if (selectedRow) {
			partitionID = selectedRow->ID();
			parentID = selectedRow->ParentID();
		}
	}

	_SetToDiskAndPartition(diskID, partitionID, parentID);
}


void
MainWindow::_SetToDiskAndPartition(partition_id disk, partition_id partition,
	partition_id parent)
{
//printf("MainWindow::_SetToDiskAndPartition(disk: %ld, partition: %ld, "
//	"parent: %ld)\n", disk, partition, parent);

	BDiskDevice* oldDisk = NULL;
	if (!fCurrentDisk || fCurrentDisk->ID() != disk) {
		oldDisk = fCurrentDisk;
		fCurrentDisk = NULL;
		if (disk >= 0) {
			BDiskDevice* newDisk = new BDiskDevice();
			status_t status = newDisk->SetTo(disk);
			if (status != B_OK) {
				printf("error switching disks: %s\n", strerror(status));
				delete newDisk;
			} else
				fCurrentDisk = newDisk;
		}
	}

	fCurrentPartitionID = partition;

	fDiskView->SetDisk(fCurrentDisk, fCurrentPartitionID);
	_UpdateMenus(fCurrentDisk, fCurrentPartitionID, parent);

	delete oldDisk;
}


void
MainWindow::_UpdateMenus(BDiskDevice* disk,
	partition_id selectedPartition, partition_id parentID)
{
	while (BMenuItem* item = fFormatMenu->RemoveItem((int32)0))
		delete item;
	while (BMenuItem* item = fDiskInitMenu->RemoveItem((int32)0))
		delete item;
	while (BMenuItem* item = fFormatContextMenuItem->RemoveItem((int32)0))
		delete item;

	fCreateMenuItem->SetEnabled(false);
	fUnmountMenuItem->SetEnabled(false);
	fDiskInitMenu->SetEnabled(false);
	fFormatMenu->SetEnabled(false);

	fCreateContextMenuItem->SetEnabled(false);
	fUnmountContextMenuItem->SetEnabled(false);
	fFormatContextMenuItem->SetEnabled(false);

	if (disk == NULL) {
		fWipeMenuItem->SetEnabled(false);
		fEjectMenuItem->SetEnabled(false);
		fSurfaceTestMenuItem->SetEnabled(false);
		fUnRegisterMenuItem->SetEnabled(false);
		fSaveMenuItem->SetEnabled(false);
		fWriteMenuItem->SetEnabled(false);
		fOpenDiskProbeMenuItem->SetEnabled(false);
		fOpenDiskProbeContextMenuItem->SetEnabled(false);
	} else {
//		fWipeMenuItem->SetEnabled(true);
		fWipeMenuItem->SetEnabled(false);
		fEjectMenuItem->SetEnabled(disk->IsRemovableMedia());
//		fSurfaceTestMenuItem->SetEnabled(true);
		fSurfaceTestMenuItem->SetEnabled(false);

		fUnRegisterMenuItem->SetEnabled(disk->IsFile());

		fSaveMenuItem->SetEnabled(true);
		fWriteMenuItem->SetEnabled(!disk->IsReadOnly());

		// Create menu and items
		BPartition* parentPartition = NULL;
		if (selectedPartition <= -2) {
			// a partitionable space item is selected
			parentPartition = disk->FindDescendant(parentID);
		}

		if (parentPartition && parentPartition->ContainsPartitioningSystem()) {
			fCreateMenuItem->SetEnabled(true);
			fCreateContextMenuItem->SetEnabled(true);
		}
		bool prepared = disk->PrepareModifications() == B_OK;

		fFormatMenu->SetEnabled(prepared);
		fDeleteMenuItem->SetEnabled(prepared);
		fChangeMenuItem->SetEnabled(prepared);

		fFormatContextMenuItem->SetEnabled(prepared);
		fDeleteContextMenuItem->SetEnabled(prepared);
		fChangeContextMenuItem->SetEnabled(prepared);

		BPartition* partition = disk->FindDescendant(selectedPartition);

		BDiskSystem diskSystem;
		fDiskDeviceRoster.RewindDiskSystems();
		while (fDiskDeviceRoster.GetNextDiskSystem(&diskSystem) == B_OK) {
			if (!diskSystem.SupportsInitializing())
				continue;

			BMessage* message = new BMessage(MSG_INITIALIZE);
			message->AddInt32("parent id", parentID);
			message->AddString("disk system", diskSystem.PrettyName());

			BString label = diskSystem.PrettyName();
			label << B_UTF8_ELLIPSIS;
			BMenuItem* item = new BMenuItem(label.String(), message);

			// TODO: Very unintuitive that we have to use PrettyName (vs Name)
			item->SetEnabled(partition != NULL
				&& partition->CanInitialize(diskSystem.PrettyName()));

			if (disk->ID() == selectedPartition
				&& !diskSystem.IsFileSystem()) {
				// Disk is selected, and DiskSystem is a partition map
				fDiskInitMenu->AddItem(item);
			} else if (diskSystem.IsFileSystem()) {
				// Otherwise a filesystem
				fFormatMenu->AddItem(item);

				// Context menu
				BMessage* message = new BMessage(MSG_INITIALIZE);
				message->AddInt32("parent id", parentID);
				message->AddString("disk system", diskSystem.PrettyName());
				BMenuItem* popUpItem = new BMenuItem(label.String(), message);
				popUpItem->SetEnabled(partition != NULL
					&& partition->CanInitialize(diskSystem.PrettyName()));
				fFormatContextMenuItem->AddItem(popUpItem);
				fFormatContextMenuItem->SetTargetForItems(this);
			}
		}

		// Mount items
		if (partition != NULL) {
			bool writable = !partition->IsReadOnly()
				&& partition->Device()->HasMedia();
			bool notMountedAndWritable = !partition->IsMounted() && writable;
			bool canChangeParameters = partition->CanEditParameters()
				|| partition->CanSetType() || partition->CanSetName();

			fFormatMenu->SetEnabled(writable && fFormatMenu->CountItems() > 0);

			fDiskInitMenu->SetEnabled(notMountedAndWritable
				&& partition->IsDevice()
				&& fDiskInitMenu->CountItems() > 0);

			fChangeMenuItem->SetEnabled(writable && canChangeParameters);
			fChangeContextMenuItem->SetEnabled(writable && canChangeParameters);

			fDeleteMenuItem->SetEnabled(notMountedAndWritable
				&& !partition->IsDevice());
			fDeleteContextMenuItem->SetEnabled(notMountedAndWritable
				&& !partition->IsDevice());

			fMountMenuItem->SetEnabled(!partition->IsMounted());
			fMountContextMenuItem->SetEnabled(!partition->IsMounted());

			fFormatContextMenuItem->SetEnabled(notMountedAndWritable
				&& fFormatContextMenuItem->CountItems() > 0);

			bool unMountable = false;
			if (partition->IsMounted()) {
				// see if this partition is the boot volume
				BVolume volume;
				BVolume bootVolume;
				if (BVolumeRoster().GetBootVolume(&bootVolume) == B_OK
					&& partition->GetVolume(&volume) == B_OK) {
					unMountable = volume != bootVolume;
				} else
					unMountable = true;
			}
			fUnmountMenuItem->SetEnabled(unMountable);
			fUnmountContextMenuItem->SetEnabled(unMountable);
		} else {
			fDeleteMenuItem->SetEnabled(false);
			fChangeMenuItem->SetEnabled(false);
			fMountMenuItem->SetEnabled(false);
			fFormatMenu->SetEnabled(false);
			fDiskInitMenu->SetEnabled(false);

			fDeleteContextMenuItem->SetEnabled(false);
			fChangeContextMenuItem->SetEnabled(false);
			fMountContextMenuItem->SetEnabled(false);
			fFormatContextMenuItem->SetEnabled(false);
		}

		if (prepared)
			disk->CancelModifications();

		fOpenDiskProbeMenuItem->SetEnabled(true);
		fOpenDiskProbeContextMenuItem->SetEnabled(true);

		fMountAllMenuItem->SetEnabled(true);
	}
	if (selectedPartition < 0) {
		fDeleteMenuItem->SetEnabled(false);
		fChangeMenuItem->SetEnabled(false);
		fMountMenuItem->SetEnabled(false);

		fDeleteContextMenuItem->SetEnabled(false);
		fChangeContextMenuItem->SetEnabled(false);
		fMountContextMenuItem->SetEnabled(false);
	}
}


void
MainWindow::_DisplayPartitionError(BString _message,
	const BPartition* partition, status_t error) const
{
	char message[1024];

	if (partition && _message.FindFirst("%s") >= 0) {
		BString name;
		name << "\"" << partition->ContentName() << "\"";
		snprintf(message, sizeof(message), _message.String(), name.String());
	} else {
		_message.ReplaceAll("%s", "");
		strlcpy(message, _message.String(), sizeof(message));
	}

	if (error < B_OK) {
		BString helper = message;
		const char* errorString
			= B_TRANSLATE_COMMENT("Error:", "in any error alert");
		snprintf(message, sizeof(message), "%s\n\n%s %s", helper.String(),
			errorString, strerror(error));
	}

	BAlert* alert = new BAlert("error", message, B_TRANSLATE("OK"), NULL, NULL,
		B_WIDTH_FROM_WIDEST, error < B_OK ? B_STOP_ALERT : B_INFO_ALERT);
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->Go(NULL);
}


// #pragma mark -


void
MainWindow::_Mount(BDiskDevice* disk, partition_id selectedPartition)
{
	if (!disk || selectedPartition < 0) {
		_DisplayPartitionError(B_TRANSLATE("You need to select a partition "
			"entry from the list."));
		return;
	}

	BPartition* partition = disk->FindDescendant(selectedPartition);
	if (!partition) {
		_DisplayPartitionError(B_TRANSLATE("Unable to find the selected "
			"partition by ID."));
		return;
	}

	if (!partition->IsMounted()) {
		status_t status = partition->Mount();
		if (status != B_OK) {
			_DisplayPartitionError(B_TRANSLATE("Could not mount partition %s."),
				partition, status);
		} else {
			// successful mount, adapt to the changes
			_ScanDrives();
		}
	} else {
		_DisplayPartitionError(
			B_TRANSLATE("The partition %s is already mounted."), partition);
	}
}


void
MainWindow::_Unmount(BDiskDevice* disk, partition_id selectedPartition)
{
	if (!disk || selectedPartition < 0) {
		_DisplayPartitionError(B_TRANSLATE("You need to select a partition "
			"entry from the list."));
		return;
	}

	BPartition* partition = disk->FindDescendant(selectedPartition);
	if (!partition) {
		_DisplayPartitionError(B_TRANSLATE("Unable to find the selected "
			"partition by ID."));
		return;
	}

	if (partition->IsMounted()) {
		BPath path;
		partition->GetMountPoint(&path);
		status_t status = partition->Unmount();
		if (status != B_OK) {
			BString message = B_TRANSLATE("Could not unmount partition");
			message << " \"" << partition->ContentName() << "\":\n\t"
				<< strerror(status) << "\n\n"
				<< B_TRANSLATE("Should unmounting be forced?\n\n"
				"Note: If an application is currently writing to the volume, "
				"unmounting it now might result in loss of data.\n");

			BAlert* alert = new BAlert(B_TRANSLATE("Force unmount"), message,
				B_TRANSLATE("Cancel"), B_TRANSLATE("Force unmount"), NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);

			if (alert->Go() == 1)
				status = partition->Unmount(B_FORCE_UNMOUNT);
			else
				return;
		}

		if (status != B_OK) {
			_DisplayPartitionError(
				B_TRANSLATE("Could not unmount partition %s."),
				partition, status);
		} else {
			if (dev_for_path(path.Path()) == dev_for_path("/"))
				rmdir(path.Path());
			// successful unmount, adapt to the changes
			_ScanDrives();
		}
	} else {
		_DisplayPartitionError(
			B_TRANSLATE("The partition %s is already unmounted."),
			partition);
	}
}


void
MainWindow::_MountAll()
{
	MountAllVisitor visitor;
	fDiskDeviceRoster.VisitEachPartition(&visitor);
}


// #pragma mark -


void
MainWindow::_Initialize(BDiskDevice* disk, partition_id selectedPartition,
	const BString& diskSystemName)
{
	if (!disk || selectedPartition < 0) {
		_DisplayPartitionError(B_TRANSLATE("You need to select a partition "
			"entry from the list."));
		return;
	}

	if (disk->IsReadOnly()) {
		_DisplayPartitionError(B_TRANSLATE("The selected disk is read-only."));
		return;
	}

	BPartition* partition = disk->FindDescendant(selectedPartition);
	if (!partition) {
		_DisplayPartitionError(B_TRANSLATE("Unable to find the selected "
			"partition by ID."));
		return;
	}

	// Unmount the partition and, when a whole device was selected, everything
	// on it: initializing a device fails while any partition it contains is
	// still mounted, and on a disk that came from another operating system
	// something usually is -- an EFI system partition, for instance.
	if (_UnmountAll(partition) != B_OK) {
		// Probably it's the system partition
		_DisplayPartitionError(
			B_TRANSLATE("The partition cannot be unmounted."));

		return;
	}

	BDiskSystem diskSystem;
	fDiskDeviceRoster.RewindDiskSystems();
	bool found = false;
	while (fDiskDeviceRoster.GetNextDiskSystem(&diskSystem) == B_OK) {
		if (diskSystem.SupportsInitializing()) {
			if (diskSystemName == diskSystem.PrettyName()) {
				found = true;
				break;
			}
		}
	}

	if (!found) {
		_DisplayPartitionError(B_TRANSLATE("Disk system %s not found!"));
		return;
	}

	BString message;

	if (diskSystem.IsFileSystem()) {
		BString intelExtendedPartition = "Intel Extended Partition";
		if (disk->ID() == selectedPartition) {
			// Formatting a whole disk with a file system leaves it with no
			// partition table, which EFI firmware cannot boot from. When the
			// disk is empty anyway, offer the layout that can be booted
			// instead of just warning about it.
			BAlert* rawAlert = new BAlert("raw disk",
				B_TRANSLATE("Formatting the whole disk leaves it without a "
					"partition table, which EFI firmware cannot boot from.\n\n"
					"Set the disk up for booting Haiku instead? That writes a "
					"GUID partition table, a 64 MiB EFI system partition for "
					"the boot loader, and gives the rest to Be File System.\n\n"
					"Either way, everything on the disk is lost."),
				B_TRANSLATE("Set up for booting"),
				B_TRANSLATE("Format the whole disk"), B_TRANSLATE("Cancel"),
				B_WIDTH_FROM_WIDEST, B_IDEA_ALERT);
			rawAlert->SetShortcut(2, B_ESCAPE);
			int32 rawChoice = rawAlert->Go();
			if (rawChoice == 2)
				return;
			if (rawChoice == 0) {
				if (_UnmountAll(partition) != B_OK) {
					_DisplayPartitionError(B_TRANSLATE("The partition "
						"cannot be unmounted."));
					return;
				}
				status_t status = _InitializeGpt(disk);
				if (status != B_OK) {
					_DisplayPartitionError(B_TRANSLATE("Could not write a "
						"GUID partition table to the disk."), NULL, status);
					return;
				}
				disk->Update();
				_CreateBootLayout(disk);
				return;
			}

			message = B_TRANSLATE("Are you sure you "
				"want to format a raw disk? (Most people initialize the disk "
				"with a partitioning system first) You will be asked "
				"again before changes are written to the disk.");
		} else if (partition->ContentName()
			&& strlen(partition->ContentName()) > 0) {
			message = B_TRANSLATE("Are you sure you "
				"want to format the partition \"%s\"? You will be asked "
				"again before changes are written to the disk.");
			message.ReplaceFirst("%s", partition->ContentName());
		} else if (partition->Type() == intelExtendedPartition) {
			message = B_TRANSLATE("Are you sure you "
				"want to format the Intel Extended Partition? Any "
				"subpartitions it contains will be overwritten if you "
				"continue. You will be asked again before changes are "
				"written to the disk.");
		} else {
			message = B_TRANSLATE("Are you sure you "
				"want to format the partition? You will be asked again "
				"before changes are written to the disk.");
		}
	} else {
		message = B_TRANSLATE("Are you sure you "
			"want to initialize the selected disk? All data will be lost. "
			"You will be asked again before changes are written to the "
			"disk.\n");
	}
	BAlert* alert = new BAlert("first notice", message.String(),
		B_TRANSLATE("Continue"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	int32 choice = alert->Go();

	if (choice == 1)
		return;

	ModificationPreparer modificationPreparer(disk);
	status_t status = modificationPreparer.ModificationStatus();
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("There was an error preparing the "
			"disk for modifications."), NULL, status);
		return;
	}

	BString name;
	BString parameters;
	InitParametersPanel* panel = new InitParametersPanel(this, diskSystemName,
		partition);
	if (panel->Go(name, parameters) != B_OK)
		return;

	bool supportsName = diskSystem.SupportsContentName();
	BString validatedName(name);
	status = partition->ValidateInitialize(diskSystem.PrettyName(),
		supportsName ? &validatedName : NULL, parameters.String());
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Validation of the given "
			"initialization parameters failed."), partition, status);
		return;
	}

	BString previousName = partition->ContentName();

	status = partition->Initialize(diskSystem.PrettyName(),
		supportsName ? validatedName.String() : NULL, parameters.String());
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Initialization of the partition "
			"%s failed. (Nothing has been written to disk.)"), partition,
			status);
		return;
	}

	// Also set the partition name in the partition table if supported
	if (partition->CanSetName()
		&& partition->ValidateSetName(&validatedName) == B_OK) {
		partition->SetName(validatedName.String());
	}

	// everything looks fine, we are ready to actually write the changes
	// to disk

	// Warn the user one more time...
	if (previousName.Length() > 0) {
		if (partition->IsDevice()) {
			message = B_TRANSLATE("Are you sure you "
				"want to write the changes back to disk now?\n\n"
				"All data on the disk \"%s\" will be irretrievably lost if you "
				"do so!");
			message.ReplaceFirst("%s", previousName.String());
		} else {
			message = B_TRANSLATE("Are you sure you "
				"want to write the changes back to disk now?\n\n"
				"All data on the partition \"%s\" will be irretrievably lost if you "
				"do so!");
			message.ReplaceFirst("%s", previousName.String());
		}
	} else {
		if (partition->IsDevice()) {
			message = B_TRANSLATE("Are you sure you "
				"want to write the changes back to disk now?\n\n"
				"All data on the selected disk will be irretrievably lost if "
				"you do so!");
		} else {
			message = B_TRANSLATE("Are you sure you "
				"want to write the changes back to disk now?\n\n"
				"All data on the selected partition will be irretrievably lost "
				"if you do so!");
		}
	}
	alert = new BAlert("final notice", message.String(),
		B_TRANSLATE("Write changes"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	choice = alert->Go();

	if (choice == 1)
		return;

	// commit
	status = modificationPreparer.CommitModifications();

	// The partition pointer is toast now! Use the partition ID to
	// retrieve it again.
	partition = disk->FindDescendant(selectedPartition);

	if (status == B_OK) {
		if (diskSystem.IsFileSystem()) {
			_DisplayPartitionError(B_TRANSLATE("The partition %s has been "
				"successfully formatted.\n"), partition);
		} else {
			_DisplayPartitionError(B_TRANSLATE("The disk has been "
				"successfully initialized.\n"), partition);

			// A freshly partitioned disk is empty, and laying it out for
			// booting Haiku by hand is fiddly: EFI firmware needs its loader
			// on a FAT32 "EFI system data" partition, and the Installer only
			// offers a partition that is exactly that. Offer to do it here.
			if (strcmp(diskSystem.PrettyName(), kPartitionTypeEFI) == 0
				&& partition != NULL && partition->IsDevice()) {
				BAlert* layoutAlert = new BAlert("boot layout",
					B_TRANSLATE("Set this disk up for booting Haiku?\n\n"
						"This creates a 64 MiB EFI system partition for the "
						"boot loader, and gives the rest of the disk to a Be "
						"File System partition to install onto.\n\n"
						"You can also create the partitions yourself."),
					B_TRANSLATE("Set up"), B_TRANSLATE("Not now"), NULL,
					B_WIDTH_FROM_WIDEST, B_IDEA_ALERT);
				layoutAlert->SetShortcut(1, B_ESCAPE);
				if (layoutAlert->Go() == 0)
					_CreateBootLayout(disk);
			}
		}
	} else {
		if (diskSystem.IsFileSystem()) {
			_DisplayPartitionError(B_TRANSLATE("Failed to format the "
				"partition %s!\n"), partition, status);
		} else {
			_DisplayPartitionError(B_TRANSLATE("Failed to initialize the "
				"disk %s!\n"), partition, status);
		}
	}

	_ScanDrives();
}


/*!	Writes an empty GUID partition table to \a disk. */
status_t
MainWindow::_InitializeGpt(BDiskDevice* disk)
{
	BDiskSystem gptSystem;
	fDiskDeviceRoster.RewindDiskSystems();
	bool found = false;
	while (fDiskDeviceRoster.GetNextDiskSystem(&gptSystem) == B_OK) {
		if (gptSystem.SupportsInitializing()
			&& strcmp(gptSystem.PrettyName(), kPartitionTypeEFI) == 0) {
			found = true;
			break;
		}
	}
	if (!found)
		return B_ENTRY_NOT_FOUND;

	ModificationPreparer modificationPreparer(disk);
	status_t status = modificationPreparer.ModificationStatus();
	if (status != B_OK)
		return status;

	BString name;
	status = disk->ValidateInitialize(gptSystem.PrettyName(), &name, "");
	if (status == B_OK)
		status = disk->Initialize(gptSystem.PrettyName(), name.String(), "");
	if (status != B_OK)
		return status;

	return modificationPreparer.CommitModifications();
}


/*!	Unmounts \a partition and everything below it. */
status_t
MainWindow::_UnmountAll(BPartition* partition)
{
	if (partition == NULL)
		return B_BAD_VALUE;

	for (int32 i = 0; i < partition->CountChildren(); i++) {
		status_t status = _UnmountAll(partition->ChildAt(i));
		if (status != B_OK)
			return status;
	}

	if (partition->IsMounted())
		return partition->Unmount();

	return B_OK;
}


/*!	Copies the EFI loader onto the EFI system partition.

	Without this the firmware has nothing to boot: a Mac shows the folder with
	a question mark, and a PC falls through to the next device. The Installer
	can do the same from its EFI loader menu, but that is a step of its own
	that is easy to miss.

	EFI/BOOT/<arch>.EFI is the path a UEFI firmware falls back to when no boot
	entry names a loader, which is what makes the disk boot by itself. It is
	the only place the loader may go: the boot loader takes any volume with a
	top-level "system" directory for a Haiku volume, and FAT does not
	distinguish case, so laying out Apple's \System\Library\CoreServices here
	would make it pick the EFI partition and leave the kernel with nothing to
	mount.
*/
status_t
MainWindow::_InstallEfiLoader(BPartition* partition)
{
	if (kEfiLoaderName == NULL)
		return B_NOT_SUPPORTED;

	BPath loaderPath;
	if (find_directory(B_SYSTEM_DATA_DIRECTORY, &loaderPath) != B_OK
		|| loaderPath.Append("platform_loaders/haiku_loader.efi") != B_OK) {
		return B_ENTRY_NOT_FOUND;
	}

	BFile loader(loaderPath.Path(), B_READ_ONLY);
	status_t status = loader.InitCheck();
	if (status != B_OK)
		return status;

	bool wasMounted = partition->IsMounted();
	if (!wasMounted) {
		status = partition->Mount();
		if (status < B_OK)
			return status;
	}

	BPath mountPoint;
	status = partition->GetMountPoint(&mountPoint);

	if (status == B_OK)
		status = copy_loader_to(loader, mountPoint, "EFI/BOOT", kEfiLoaderName);

	if (!wasMounted)
		partition->Unmount();

	return status;
}


/*!	Lays an empty GPT disk out the way an EFI machine needs to boot Haiku:
	a FAT32 EFI system partition for the loader, then the rest as BFS.

	One partition per modification session, each committed before the next is
	placed: the free-space list does not account for a child that has been
	created but not yet written, so sizing the second partition inside the
	same session puts it on top of the first.

	The EFI partition is deliberately formatted as FAT32 rather than letting
	the FAT driver pick by size -- at 64 MiB it would choose FAT16, and the
	Installer only offers an EFI partition whose content type is FAT32.
*/
void
MainWindow::_CreateBootLayout(BDiskDevice* disk)
{
	static const off_t kEfiPartitionSize = 64 * 1024 * 1024;
	static const char* kEfiPartitionType = "EFI system data";

	BAlert* alert = new BAlert("final notice", B_TRANSLATE("Are you sure you "
		"want to write the changes back to disk now?\n\n"
		"All data on the disk will be irretrievably lost if you do so!"),
		B_TRANSLATE("Write changes"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	if (alert->Go() == 1)
		return;

	// Two passes over the same two partitions: create them, then format
	// them. A partition cannot be given a file system before it exists.
	const char* types[2] = { kEfiPartitionType, kPartitionTypeBFS };
	const char* names[2] = { "EFI", "RenkuOS" };

	for (int32 pass = 0; pass < 2; pass++) {
		off_t wanted = pass == 0 ? kEfiPartitionSize : -1;

		ModificationPreparer modificationPreparer(disk);
		status_t status = modificationPreparer.ModificationStatus();

		BPartitioningInfo partitioningInfo;
		off_t offset = 0;
		off_t size = 0;
		if (status == B_OK)
			status = disk->GetPartitioningInfo(&partitioningInfo);
		if (status == B_OK && partitioningInfo.CountPartitionableSpaces() == 0)
			status = B_ENTRY_NOT_FOUND;
		if (status == B_OK) {
			// Take the largest free span, so a small gap left in front of the
			// first partition does not get picked for the big one.
			off_t bestOffset = 0;
			off_t bestSize = 0;
			for (int32 i = 0;
				i < partitioningInfo.CountPartitionableSpaces(); i++) {
				off_t spaceOffset;
				off_t spaceSize;
				if (partitioningInfo.GetPartitionableSpaceAt(i, &spaceOffset,
						&spaceSize) != B_OK) {
					continue;
				}
				if (spaceSize > bestSize) {
					bestOffset = spaceOffset;
					bestSize = spaceSize;
				}
			}
			if (bestSize == 0)
				status = B_ENTRY_NOT_FOUND;

			// Start on a 1 MiB boundary, the alignment every other
			// partitioning tool uses. The first usable GPT block is 34,
			// which is neither aligned to a 4 KiB physical sector nor
			// somewhere other systems expect a partition to begin.
			static const off_t kAlignment = 1024 * 1024;
			off_t alignedOffset = (bestOffset + kAlignment - 1)
				/ kAlignment * kAlignment;
			if (alignedOffset - bestOffset < bestSize)
				bestSize -= alignedOffset - bestOffset;
			else
				bestSize = 0;
			bestOffset = alignedOffset;

			// And keep the size a whole number of MiB.
			bestSize = bestSize / kAlignment * kAlignment;

			if (bestSize == 0)
				status = B_ENTRY_NOT_FOUND;
			offset = bestOffset;
			size = wanted > 0 && wanted < bestSize ? wanted : bestSize;
		}

		if (status == B_OK && pass == 0 && size < kEfiPartitionSize) {
			_DisplayPartitionError(B_TRANSLATE("This disk is too small to lay "
				"out for booting."));
			return;
		}

		BString name(names[pass]);
		if (status == B_OK) {
			status = disk->ValidateCreateChild(&offset, &size, types[pass],
				&name, "");
		}
		if (status == B_OK) {
			status = disk->CreateChild(offset, size, types[pass],
				name.String(), "");
		}
		if (status == B_OK)
			status = modificationPreparer.CommitModifications();

		if (status != B_OK) {
			_DisplayPartitionError(pass == 0
				? B_TRANSLATE("Could not create the EFI system partition.")
				: B_TRANSLATE("Could not create the Haiku partition."),
				NULL, status);
			_ScanDrives();
			return;
		}

		disk->Update();
	}

	// Now give each of them a file system.
	for (int32 i = 0; i < disk->CountChildren(); i++) {
		BPartition* child = disk->ChildAt(i);
		if (child == NULL || child->Type() == NULL)
			continue;

		const char* diskSystem = NULL;
		const char* parameters = "";
		BString name;
		if (strcmp(child->Type(), kEfiPartitionType) == 0) {
			diskSystem = kPartitionTypeFAT32;
			parameters = "fat 32";
			name = "EFI";
		} else if (strcmp(child->Type(), kPartitionTypeBFS) == 0) {
			diskSystem = kPartitionTypeBFS;
			name = "RenkuOS";
		} else
			continue;

		ModificationPreparer modificationPreparer(disk);
		status_t status = modificationPreparer.ModificationStatus();
		if (status == B_OK) {
			status = child->ValidateInitialize(diskSystem, &name, parameters);
			if (status == B_OK)
				status = child->Initialize(diskSystem, name.String(), parameters);
			if (status == B_OK)
				status = modificationPreparer.CommitModifications();
		}

		if (status != B_OK) {
			_DisplayPartitionError(B_TRANSLATE("The partitions were created, "
				"but one of them could not be formatted. Format it from the "
				"Partition menu."), NULL, status);
			_ScanDrives();
			return;
		}

		disk->Update();
	}

	// The loader has to go on after the format, not before: initializing the
	// partition would throw it away again.
	status_t loaderStatus = B_ENTRY_NOT_FOUND;
	for (int32 i = 0; i < disk->CountChildren(); i++) {
		BPartition* child = disk->ChildAt(i);
		if (child == NULL || child->Type() == NULL
			|| strcmp(child->Type(), kEfiPartitionType) != 0) {
			continue;
		}

		loaderStatus = _InstallEfiLoader(child);
		break;
	}

	if (loaderStatus != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("The partitions were created, but "
			"the boot loader could not be copied to the EFI partition. The "
			"Installer can put it there from its EFI loader menu."), NULL,
			loaderStatus);
		_ScanDrives();
		return;
	}

	_DisplayPartitionError(B_TRANSLATE("The disk is ready. The boot loader is "
		"already on the EFI partition, so installing onto the Haiku partition "
		"is all that is left."));

	_ScanDrives();
}


void
MainWindow::_Create(BDiskDevice* disk, partition_id selectedPartition)
{
	if (!disk || selectedPartition > -2) {
		_DisplayPartitionError(B_TRANSLATE("The currently selected partition "
			"is not empty."));
		return;
	}

	if (disk->IsReadOnly()) {
		_DisplayPartitionError(B_TRANSLATE("The selected disk is read-only."));
		return;
	}

	PartitionListRow* currentSelection = dynamic_cast<PartitionListRow*>(
		fListView->CurrentSelection());
	if (!currentSelection) {
		_DisplayPartitionError(B_TRANSLATE("There was an error acquiring the "
			"partition row."));
		return;
	}

	BPartition* parent = disk->FindDescendant(currentSelection->ParentID());
	if (!parent) {
		_DisplayPartitionError(B_TRANSLATE("The currently selected partition "
			"does not have a parent partition."));
		return;
	}

	if (!parent->ContainsPartitioningSystem()) {
		_DisplayPartitionError(B_TRANSLATE("The selected partition does not "
			"contain a partitioning system."));
		return;
	}

	ModificationPreparer modificationPreparer(disk);
	status_t status = modificationPreparer.ModificationStatus();
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("There was an error preparing the "
			"disk for modifications."), NULL, status);
		return;
	}

	// get partitioning info
	BPartitioningInfo partitioningInfo;
	status_t error = parent->GetPartitioningInfo(&partitioningInfo);
	if (error != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Could not acquire partitioning "
			"information."));
		return;
	}

	int32 spacesCount = partitioningInfo.CountPartitionableSpaces();
	if (spacesCount == 0) {
		_DisplayPartitionError(B_TRANSLATE("There's no space on the partition "
			"where a child partition could be created."));
		return;
	}

	BString name, type, parameters;
	off_t offset = currentSelection->Offset();
	off_t size = currentSelection->Size();

	CreateParametersPanel* panel = new CreateParametersPanel(this, parent,
		offset, size);
	status = panel->Go(offset, size, name, type, parameters);
	if (status != B_OK) {
		if (status != B_CANCELED) {
			_DisplayPartitionError(B_TRANSLATE("The panel could not return "
				"successfully."), NULL, status);
		}
		return;
	}

	status = parent->ValidateCreateChild(&offset, &size, type.String(),
		&name, parameters.String());

	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Validation of the given creation "
			"parameters failed."), NULL, status);
		return;
	}

	// Warn the user one more time...
	BAlert* alert = new BAlert("final notice", B_TRANSLATE("Are you sure you "
		"want to write the changes back to disk now?\n\n"
		"All data on the partition will be irretrievably lost if you do "
		"so!"), B_TRANSLATE("Write changes"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	int32 choice = alert->Go();

	if (choice == 1)
		return;

	status = parent->CreateChild(offset, size, type.String(), name.String(),
		parameters.String());

	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Creation of the partition has "
			"failed."), NULL, status);
		return;
	}

	// commit
	status = modificationPreparer.CommitModifications();

	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Failed to create the "
			"partition. No changes have been written to disk."), NULL, status);
		return;
	}

	// The disk layout has changed, update disk information
	bool updated;
	status = disk->Update(&updated);

	_ScanDrives();
	fDiskView->ForceUpdate();
}


void
MainWindow::_Delete(BDiskDevice* disk, partition_id selectedPartition)
{
	if (!disk || selectedPartition < 0) {
		_DisplayPartitionError(B_TRANSLATE("You need to select a partition "
			"entry from the list."));
		return;
	}

	if (disk->IsReadOnly()) {
		_DisplayPartitionError(B_TRANSLATE("The selected disk is read-only."));
		return;
	}

	BPartition* partition = disk->FindDescendant(selectedPartition);
	if (!partition) {
		_DisplayPartitionError(B_TRANSLATE("Unable to find the selected "
			"partition by ID."));
		return;
	}

	BPartition* parent = partition->Parent();
	if (!parent) {
		_DisplayPartitionError(B_TRANSLATE("The currently selected partition "
			"does not have a parent partition."));
		return;
	}

	ModificationPreparer modificationPreparer(disk);
	status_t status = modificationPreparer.ModificationStatus();
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("There was an error preparing the "
			"disk for modifications."), NULL, status);
		return;
	}

	if (!parent->CanDeleteChild(partition->Index())) {
		_DisplayPartitionError(
			B_TRANSLATE("Cannot delete the selected partition."));
		return;
	}

	// Warn the user one more time...
	BAlert* alert = new BAlert("final notice", B_TRANSLATE("Are you sure you "
		"want to delete the selected partition?\n\n"
		"All data on the partition will be irretrievably lost if you "
		"do so!"), B_TRANSLATE("Delete partition"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	int32 choice = alert->Go();

	if (choice == 1)
		return;

	status = parent->DeleteChild(partition->Index());
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Could not delete the selected "
			"partition."), NULL, status);
		return;
	}

	status = modificationPreparer.CommitModifications();

	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Failed to delete the partition. "
			"No changes have been written to disk."), NULL, status);
		return;
	}

	_ScanDrives();
	fDiskView->ForceUpdate();
}


void
MainWindow::_ChangeParameters(BDiskDevice* disk, partition_id selectedPartition)
{
	if (disk == NULL || selectedPartition < 0) {
		_DisplayPartitionError(B_TRANSLATE("You need to select a partition "
			"entry from the list."));
		return;
	}

	if (disk->IsReadOnly()) {
		_DisplayPartitionError(B_TRANSLATE("The selected disk is read-only."));
		return;
	}

	BPartition* partition = disk->FindDescendant(selectedPartition);
	if (partition == NULL) {
		_DisplayPartitionError(B_TRANSLATE("Unable to find the selected "
			"partition by ID."));
		return;
	}

	ModificationPreparer modificationPreparer(disk);
	status_t status = modificationPreparer.ModificationStatus();
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("There was an error preparing the "
			"disk for modifications."), NULL, status);
		return;
	}

	ChangeParametersPanel* panel = new ChangeParametersPanel(this, partition);

	BString name, type, parameters;
	status = panel->Go(name, type, parameters);
	if (status != B_OK) {
		if (status != B_CANCELED) {
			_DisplayPartitionError(B_TRANSLATE("The panel experienced a "
				"problem!"), NULL, status);
		}
		return;
	}

	if (partition->CanSetType())
		status = partition->ValidateSetType(type.String());
	if (status == B_OK && partition->CanSetName())
		status = partition->ValidateSetName(&name);
	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Validation of the given parameters "
			"failed."));
		return;
	}

	// Warn the user one more time...
	BAlert* alert = new BAlert("final notice", B_TRANSLATE("Are you sure you "
		"want to change parameters of the selected partition?\n\n"
		"The partition may no longer be recognized by other operating systems "
		"anymore!"), B_TRANSLATE("Change parameters"), B_TRANSLATE("Cancel"),
		NULL, B_WIDTH_FROM_WIDEST, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	int32 choice = alert->Go();

	if (choice == 1)
		return;

	if (partition->CanSetType())
		status = partition->SetType(type.String());
	if (status == B_OK && partition->CanSetName())
		status = partition->SetName(name.String());
	if (status == B_OK && partition->CanEditParameters())
		status = partition->SetParameters(parameters.String());

	if (status != B_OK) {
		_DisplayPartitionError(
			B_TRANSLATE("Could not change the parameters of the selected "
				"partition."), NULL, status);
		return;
	}

	status = modificationPreparer.CommitModifications();

	if (status != B_OK) {
		_DisplayPartitionError(B_TRANSLATE("Failed to change the parameters "
			"of the partition. No changes have been written to disk."), NULL,
			status);
		return;
	}

	_ScanDrives();
	fDiskView->ForceUpdate();
}


float
MainWindow::_ColumnListViewHeight(BColumnListView* list, BRow* currentRow)
{
	float height = 0;
	int32 rows = list->CountRows(currentRow);
	for (int32 i = 0; i < rows; i++) {
		BRow* row = list->RowAt(i, currentRow);
		height += row->Height() + 1;
		if (row->IsExpanded() && list->CountRows(row) > 0)
			height += _ColumnListViewHeight(list, row);
	}
	return height;
}


void
MainWindow::_UpdateWindowZoomLimits()
{
	float maxHeight = 0;
	int32 numColumns = fListView->CountColumns();
	BRow* parentRow = fListView->RowAt(0, NULL);
	BColumn* column = NULL;

	maxHeight += _ColumnListViewHeight(fListView, NULL);

	float maxWidth = fListView->LatchWidth();
	for (int32 i = 0; i < numColumns; i++) {
		column = fListView->ColumnAt(i);
		maxWidth += column->Width();
	}

	maxHeight += B_H_SCROLL_BAR_HEIGHT;
	maxHeight += 1.5 * parentRow->Height();	// the label row
	maxHeight += fDiskView->Bounds().Height();
	maxHeight += fMenuBar->Bounds().Height();
	maxWidth += 1.5 * B_V_SCROLL_BAR_WIDTH;	// scroll bar & borders

	SetZoomLimits(maxWidth, maxHeight);
}

