/*
 * Copyright 2009, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Weirauch, dev@m-phasis.de
 */
#ifndef DESKBAR_REPLICANT_H
#define DESKBAR_REPLICANT_H


#include <Message.h>
#include <View.h>


class BMessageRunner;


extern const char* kDeskbarItemName;


class DeskbarReplicant : public BView {
	public:
		DeskbarReplicant(BRect frame, int32 resizingMode);
		DeskbarReplicant(BMessage* archive);
		virtual ~DeskbarReplicant();

		static	DeskbarReplicant* Instantiate(BMessage* archive);
		virtual	status_t Archive(BMessage* archive, bool deep = true) const;

		virtual	void	AttachedToWindow();
		virtual	void	DetachedFromWindow();

		virtual	void	Draw(BRect updateRect);

		virtual	void	MessageReceived(BMessage* message);
		virtual	void	MouseDown(BPoint where);

	private:
		void			_Init();

		void			_QuitBluetoothServer();

		void			_ShowErrorAlert(BString msg, status_t status);
		void			_AskConnectedDevices();
		void			_ShowMenu(BPoint where);

		BBitmap*		fIcon;
		BMessage		fConnected;
		BMessageRunner*	fPoll;
};

#endif	// DESKBAR_REPLICANT_H
