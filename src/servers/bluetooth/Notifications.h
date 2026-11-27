/*
 * Copyright 2026, Andrea Bernardi
 * SPDX-License-Identifier: MIT
 */
#ifndef _BLUETOOTH_NOTIFICATIONS_H_
#define _BLUETOOTH_NOTIFICATIONS_H_


struct ServerRemoteDevice;


/*!	What happens to the remote devices, told with the system notifications
	instead of windows of their own. A click opens the Bluetooth preferences.
*/
namespace Notifications {

void	Connected(const ServerRemoteDevice* device);
void	Disconnected(const ServerRemoteDevice* device);
void	Paired(const ServerRemoteDevice* device, bool success);

}


#endif	// _BLUETOOTH_NOTIFICATIONS_H_
