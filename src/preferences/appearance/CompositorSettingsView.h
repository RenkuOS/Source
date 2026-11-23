/*
 * Copyright 2026, Ian Dumych, idumych@gmail.com.
 * SPDX-License-Identifier: MIT
 */
#ifndef COMPOSITOR_SETTINGS_VIEW_H
#define COMPOSITOR_SETTINGS_VIEW_H


#include <View.h>


class BCheckBox;
class BMenuField;
class BSlider;


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
			void				_SetOption(BCheckBox* checkBox, int32 code,
									bool value);
			void				_SetWobbliness(int32 wobbliness);
			void				_UpdateWobbliness();
			void				_SetGlass(int32 glass);
			void				_SetGlassOpacity(int32 opacity);
			void				_UpdateGlass();

	static	bool				_Get(int32 code, bool defaultValue);
	static	void				_Set(int32 code, bool value);
	static	uint8				_GetWobbliness();
	static	uint8				_GetValue(int32 code, uint8 defaultValue);
	static	void				_SetValue(int32 code, uint8 value);

private:
			BCheckBox*			fCompositedCheckBox;
			BCheckBox*			fShadowsCheckBox;
			BCheckBox*			fRoundedCornersCheckBox;
			BCheckBox*			fDragTransparencyCheckBox;
			BCheckBox*			fWobblyCheckBox;
			BSlider*			fWobblinessSlider;
			BMenuField*			fGlassMenuField;
			BSlider*			fGlassOpacitySlider;

			bool				fSavedComposited;
			bool				fSavedShadows;
			bool				fSavedRoundedCorners;
			bool				fSavedDragTransparency;
			bool				fSavedWobbly;
			int32				fSavedWobbliness;
			int32				fSavedGlass;
			int32				fSavedGlassOpacity;
};


#endif // COMPOSITOR_SETTINGS_VIEW_H
