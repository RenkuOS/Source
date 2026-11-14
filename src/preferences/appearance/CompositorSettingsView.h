/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef COMPOSITOR_SETTINGS_VIEW_H
#define COMPOSITOR_SETTINGS_VIEW_H


#include <View.h>


class BCheckBox;


class CompositorSettingsView : public BView {
public:
								CompositorSettingsView(const char* name);
	virtual						~CompositorSettingsView();

	virtual	void				AttachedToWindow();
	virtual	void				MessageReceived(BMessage* message);

			bool				IsDefaultable();
			void				SetDefaults();

			bool				IsRevertable();
			void				Revert();

private:
			void				_OfferRestart();
			void				_SetComposited(bool composited);

	static	bool				_Get(int32 code, bool defaultValue);
	static	void				_Set(int32 code, bool value);

private:
			BCheckBox*			fCompositedCheckBox;

			bool				fSavedComposited;
};


#endif // COMPOSITOR_SETTINGS_VIEW_H
