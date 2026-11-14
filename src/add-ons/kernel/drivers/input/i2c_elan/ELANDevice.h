/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2023 Vladimir Serbinenko <phcoder@gmail.com>
 * Distributed under the terms of the MIT license.
 */
#ifndef I2C_ELAN_DEVICE_H
#define I2C_ELAN_DEVICE_H


#include <i2c.h>
#include <keyboard_mouse_driver.h>
#include <lock.h>

#define ELAN_MAX_REPORT_LENGTH	39
#define ELAN_MAX_FINGERS		5


class InputReadyLine;

struct elan_finger {
	uint32	x;
	uint32	y;
	uint32	pressure;
	uint32	width;
};

class ELANDevice {
public:
								ELANDevice(device_node* parent,
									i2c_device_interface* i2c,
									i2c_device i2cCookie,
									InputReadyLine* inputReady);
								~ELANDevice();

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(uint32 flags);
			status_t			Close();
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			void				SetPublishPath(char *publishPath);
			const char *		PublishPath() { return fPublishPath; }
			status_t			Control(uint32 op, void *buffer, size_t length);
			device_node*		Parent() { return fParent; }

private:
			status_t			_Initialize();
			status_t			_QueryDevice();
			status_t			_Wake();
			status_t			_Sleep();

			status_t			_ReadReport(touchpad_movement* movement,
									bigtime_t timeout);
			int					_ParseReport(const uint8* report,
									touchpad_movement* movement);
			void				_TrackPosition(touchpad_movement* movement,
									int count);
			int					_ClickFingers(int count,
									const elan_finger** presser) const;
			bool				_EmulateButtons(touchpad_movement* movement,
									int count);
			void				_ResetContact();

			status_t			_ReadRegister(uint16 reg, void* value,
									size_t length);
			status_t			_WriteRegister(uint16 reg, uint16 value);
			status_t			_Receive(void* buffer, size_t length);
			status_t			_Exec(const uint8* cmd, size_t cmdLength,
									void* buffer, size_t length);

private:
			status_t			fStatus;

			int32				fOpenCount;
			bool				fRemoved;

			char *				fPublishPath;

			device_node*		fParent;

			i2c_device_interface* fI2C;
			i2c_device			fI2CCookie;
			InputReadyLine*		fInputReady;

			mutex				fReadLock;

			uint8				fPattern;
			size_t				fReportLength;
			uint16				fMaxX;
			uint16				fMaxY;
			uint8				fPressureAdjustment;

			uint8				fReport[ELAN_MAX_REPORT_LENGTH];
			uint8				fLastReport[ELAN_MAX_REPORT_LENGTH];
			uint32				fBadReports;

			elan_finger			fFingers[ELAN_MAX_FINGERS];
			elan_finger			fLastFingers[ELAN_MAX_FINGERS];
			uint32				fFingerMask;
			uint32				fLastFingerMask;
			int32				fPositionX;
			int32				fPositionY;

			// clickpad button emulation
			uint32				fClickButton;
			int					fLastCount;
			int					fContactMaxFingers;
			bigtime_t			fContactStart;
			uint32				fTapX;
			uint32				fTapY;
			bool				fContactMoved;
			bool				fContactClicked;
			bool				fIgnoreContact;
			bool				fPendingRelease;

			touchpad_specs		fHardwareSpecs;
};


#endif // I2C_ELAN_DEVICE_H
