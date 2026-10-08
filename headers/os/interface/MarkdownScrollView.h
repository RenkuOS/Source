/*
 * Copyright 2026, Pirati Del Frico
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef MARKDOWN_SCROLL_VIEW_H
#define MARKDOWN_SCROLL_VIEW_H

#include <ScrollView.h>
#include <Button.h>
#include "MarkdownView.h"

enum {
	MSG_COPY_RAW_MARKDOWN = 'mcpR',
	MSG_COPY_PLAIN_TEXT   = 'mcpT'
};

class BMarkdownScrollView : public BScrollView {
public:
	BMarkdownScrollView(const char* name, BMarkdownView* target,
		uint32 resizingMode = B_FOLLOW_LEFT | B_FOLLOW_TOP,
		uint32 flags = 0, bool horizontal = false, bool vertical = true,
		border_style border = B_FANCY_BORDER);

	virtual ~BMarkdownScrollView();

	virtual void AttachedToWindow();
	virtual void FrameResized(float width, float height);
	virtual void DoLayout();
	virtual void MessageReceived(BMessage* message);

private:
	BMarkdownView* fMarkdownTarget;
	BButton*       fCopyRawBtn;
	BButton*       fCopyTextBtn;
};

#endif // MARKDOWN_SCROLL_VIEW_H
