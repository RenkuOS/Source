/*
 * Copyright 2026, Pirati Del Frico
 * All rights reserved. Distributed under the terms of the MIT license.
 */
#include "MarkdownScrollView.h"
#include <Clipboard.h>

BMarkdownScrollView::BMarkdownScrollView(const char* name, BMarkdownView* target,
	uint32 resizingMode, uint32 flags, bool horizontal, bool vertical,
	border_style border)
	:
	BScrollView(name, target, resizingMode, flags, horizontal, vertical, border),
	fMarkdownTarget(target)
{
	// Ci assicuriamo che la ScrollView riceva gli eventi di ridimensionamento
	SetFlags(Flags() | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE);

	if (fMarkdownTarget != NULL) {
		// Il target deve ridimensionarsi insieme alla ScrollView
		fMarkdownTarget->SetResizingMode(B_FOLLOW_NONE);
	}
	// Creiamo i due pulsanti
	fCopyRawBtn = new BButton("copy_raw", "📋 Copy Raw", new BMessage(MSG_COPY_RAW_MARKDOWN));
	fCopyTextBtn = new BButton("copy_text", "📄 Copy Text", new BMessage(MSG_COPY_PLAIN_TEXT));
	fCopyRawBtn->SetResizingMode(B_FOLLOW_RIGHT | B_FOLLOW_TOP);
	fCopyTextBtn->SetResizingMode(B_FOLLOW_RIGHT | B_FOLLOW_TOP);

	// Font compatto per i pulsanti
	BFont miniFont(be_plain_font);
	miniFont.SetSize(10.0f);
	fCopyRawBtn->SetFont(&miniFont);
	fCopyTextBtn->SetFont(&miniFont);

	AddChild(fCopyRawBtn);
	AddChild(fCopyTextBtn);
}

BMarkdownScrollView::~BMarkdownScrollView()
{
}

void
BMarkdownScrollView::FrameResized(float width, float height)
{
	BScrollView::FrameResized(width, height);
	DoLayout();
}

void
BMarkdownScrollView::AttachedToWindow()
{
	BScrollView::AttachedToWindow();

	fCopyRawBtn->SetTarget(this);
	fCopyTextBtn->SetTarget(this);
	DoLayout();
}

void
BMarkdownScrollView::DoLayout()
{
	BScrollView::DoLayout();

	if (fMarkdownTarget == NULL)
		return;

	BRect bounds = Bounds();
	float toolbarHeight = 22.0f;
	float border = 2.0f; // B_FANCY_BORDER

	// Posizioniamo i pulsanti in alto a destra
	float rawWidth = fCopyRawBtn->StringWidth(fCopyRawBtn->Label()) + 12.0f;
	float textWidth = fCopyTextBtn->StringWidth(fCopyTextBtn->Label()) + 12.0f;

	float rightOffset = border + 2.0f;
	if (ScrollBar(B_VERTICAL) != NULL)
		rightOffset += ScrollBar(B_VERTICAL)->Frame().Width();

	fCopyTextBtn->MoveTo(bounds.right - rightOffset - textWidth, border + 1.0f);
	fCopyTextBtn->ResizeTo(textWidth, toolbarHeight - 2.0f);

	fCopyRawBtn->MoveTo(bounds.right - rightOffset - textWidth - rawWidth - 4.0f, border + 1.0f);
	fCopyRawBtn->ResizeTo(rawWidth, toolbarHeight - 2.0f);

	// Posizioniamo la BMarkdownView partendo da una Y fissa assoluta (border + toolbarHeight)
	float targetTop = border + toolbarHeight;
	float targetLeft = border;
	float targetWidth = bounds.Width() - (border * 2.0f);
	float targetHeight = bounds.Height() - targetTop - border;

	if (ScrollBar(B_VERTICAL) != NULL)
		targetWidth -= ScrollBar(B_VERTICAL)->Frame().Width();
	if (ScrollBar(B_HORIZONTAL) != NULL)
		targetHeight -= ScrollBar(B_HORIZONTAL)->Frame().Height();

	if (targetWidth < 10.0f) targetWidth = 10.0f;
	if (targetHeight < 10.0f) targetHeight = 10.0f;

	fMarkdownTarget->MoveTo(targetLeft, targetTop);

	// 3. Aggiorniamo la dimensione della BTextView e forziamo il ricalcolo del TextRect e della BScrollBar
	if (fMarkdownTarget->Bounds().Width() != targetWidth
		|| fMarkdownTarget->Bounds().Height() != targetHeight) {

		fMarkdownTarget->ResizeTo(targetWidth, targetHeight);

		// Aggiorniamo l'area interna di wrapping del testo per BTextView
		BRect textRect(5.0f, 5.0f, targetWidth - 5.0f, targetHeight - 5.0f);
		fMarkdownTarget->SetTextRect(textRect);

		// Notifichiamo la BMarkdownView affinché ricalcoli il layout
		fMarkdownTarget->FrameResized(targetWidth, targetHeight);
	}
}

void
BMarkdownScrollView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_COPY_RAW_MARKDOWN: {
			if (fMarkdownTarget != NULL)
				fMarkdownTarget->CopyRawMarkdownToClipboard();
			break;
		}

		case MSG_COPY_PLAIN_TEXT: {
			if (fMarkdownTarget != NULL)
				fMarkdownTarget->CopyPlainTextToClipboard();
			break;
		}

		default:
			BScrollView::MessageReceived(message);
			break;
	}
}
