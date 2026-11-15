/*
 * Copyright 2019, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Author:
 *		Preetpal Kaur <preetpalok123@gmail.com>
 */


#ifndef INPUT_WINDOW_H
#define INPUT_WINDOW_H


#include <CardView.h>
#include <Input.h>
#include <ListView.h>
#include <Message.h>
#include <String.h>
#include <Window.h>

#include "MouseSettings.h"


class InputWindow : public BWindow
{
public:
							InputWindow(BRect rect);
		void				MessageReceived(BMessage* message);
		void				Show();
		void				Hide();

private:
		status_t			FindDevice();
		void				AddDevice(BInputDevice* device);
		bool				_SelectDevice(const char* name);

private:
		BListView*			fDeviceListView;
		BCardView*			fCardView;

		MultipleMouseSettings 	fMultipleMouseSettings;
		BString				fDeviceToSelect;
				// asked for before the device was there
};

#endif /* INPUT_WINDOW_H */
