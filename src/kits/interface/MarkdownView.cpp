#include "MarkdownView.h"

#include <InterfaceDefs.h>
#include <algorithm>

BMarkdownView::BMarkdownView(const char* name, uint32 flags)
	:
	BTextView(name, flags),
	fCodeBlocks(20)
{
	fRawMarkdown.SetTo("");
	MakeEditable(false);
	MakeSelectable(true);
	SetStylable(true);
}

BMarkdownView::BMarkdownView(const char* name, const BFont* font,
	const rgb_color* color, uint32 flags)
	:
	BTextView(name, font, color, flags),
	fCodeBlocks(20)
{
	fRawMarkdown.SetTo("");
	MakeEditable(false);
	MakeSelectable(true);
	SetStylable(true);
}

BMarkdownView::~BMarkdownView()
{
}
/* niente riquadro perché viene sovrascritto
void
BMarkdownView::Draw(BRect updateRect)
{
	PushState();

	rgb_color docBg = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	rgb_color docText = ui_color(B_DOCUMENT_TEXT_COLOR);

	// Luma formula: 0.299 R + 0.587 G + 0.114 B
	float luminance = (0.299f * docBg.red + 0.587f * docBg.green + 0.114f * docBg.blue);

	// Invertiamo lo sfondo del riquadro rispetto allo sfondo della finestra
	rgb_color blockBgColor;
	if (luminance >= 128.0f) {
		// Tema Chiaro -> Riquadro Scuro
		blockBgColor = (rgb_color){ 35, 38, 41, 255 };
	} else {
		// Tema Scuro -> Riquadro Chiaro
		blockBgColor = (rgb_color){ 235, 238, 242, 255 };
	}

	SetHighColor(blockBgColor);

	int32 count = fCodeBlocks.CountItems();
	for (int32 i = 0; i < count; i++) {
		CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
		if (block == NULL || block->startPos >= block->endPos)
			continue;

		BPoint startPt = PointAt(block->startPos);
		BPoint endPt = PointAt(block->endPos);
		
		BRect blockRect;
		blockRect.left = 0.0f;
		blockRect.right = Bounds().Width();
		blockRect.top = startPt.y - 1.0f;
		blockRect.bottom = endPt.y + LineHeight(block->endPos) + 1.0f;

		blockRect.InsetBy(2.0f, 0.0f);

		if (blockRect.Intersects(updateRect)) {
			FillRoundRect(blockRect, 4.0f, 4.0f);
		}
	}

	PopState();

	// Disegna il testo nativo della BTextView e le selezioni sopra lo sfondo
	//BTextView::Draw(updateRect);
	PushState();
	SetDrawingMode(B_OP_OVER);
	
	// Ora BTextView disegna solo i glyph dei caratteri senza cancellare lo sfondo sotto
	BTextView::Draw(updateRect);
	
	PopState();
}*/
/* ancora non ci siamo
void
BMarkdownView::Draw(BRect updateRect)
{
	// 1. Prima facciamo disegnare il testo e lo sfondo base a BTextView
	BTextView::Draw(updateRect);

	// 2. Disegniamo i riquadri sopra con la modalità di blend appropriata
	PushState();

	rgb_color docBg = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	float luminance = (0.299f * docBg.red + 0.587f * docBg.green + 0.114f * docBg.blue);

	if (luminance >= 128.0f) {
		// Tema Chiaro: B_OP_MIN fonde il riquadro scuro mantenendo il testo scuro
		SetDrawingMode(B_OP_MAX);
		SetHighColor((rgb_color){ 40, 44, 52, 255 });
	} else {
		// Tema Scuro: B_OP_MAX fonde il riquadro chiaro mantenendo il testo chiaro
		SetDrawingMode(B_OP_MIN);
		SetHighColor((rgb_color){ 220, 224, 230, 255 });
	}

	int32 count = fCodeBlocks.CountItems();
	for (int32 i = 0; i < count; i++) {
		CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
		if (block == NULL || block->startPos >= block->endPos)
			continue;

		BPoint startPt = PointAt(block->startPos);

		// Correggiamo l'offset di fine per rimanere dentro l'ultima riga del blocco
		int32 endPosAdjusted = std::max(block->startPos, block->endPos - 1);
		BPoint endPt = PointAt(endPosAdjusted);

		BRect blockRect;
		blockRect.left = 0.0f;
		blockRect.right = Bounds().Width();
		blockRect.top = startPt.y - 1.0f;
		blockRect.bottom = endPt.y + LineHeight(endPosAdjusted) + 1.0f;

		blockRect.InsetBy(2.0f, 0.0f);

		if (blockRect.Intersects(updateRect)) {
			FillRoundRect(blockRect, 4.0f, 4.0f);
		}
	}

	PopState();
}*/
void
BMarkdownView::Draw(BRect updateRect)
{
	// 1. BTextView disegna tutto il testo (compreso il testo chiaro del codice)
	// ma lo fa sullo sfondo bianco standard del documento.
	BTextView::Draw(updateRect);

	int32 count = fCodeBlocks.CountItems();
	if (count == 0)
		return;

	PushState();

	rgb_color docBg = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	float luminance = (0.299f * docBg.red + 0.587f * docBg.green + 0.114f * docBg.blue);

	// Sfondo del riquadro invertito
	rgb_color blockBgColor;
	rgb_color codeTextColor;
	
	if (luminance >= 128.0f) {
		// Tema Chiaro -> Riquadro Scuro, Testo Chiaro
		blockBgColor  = (rgb_color){ 35, 38, 41, 255 };
		codeTextColor = (rgb_color){ 235, 238, 242, 255 };
	} else {
		// Tema Scuro -> Riquadro Chiaro/Giallino, Testo Scuro
		blockBgColor  = (rgb_color){ 245, 242, 220, 255 };
		codeTextColor = (rgb_color){ 25, 25, 25, 255 };
	}	
	
	for (int32 i = 0; i < count; i++) {
		CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
		if (block == NULL || block->startPos >= block->endPos)
			continue;

		BPoint startPt = PointAt(block->startPos);
		int32 endPosAdjusted = std::max(block->startPos, block->endPos - 1);
		BPoint endPt = PointAt(endPosAdjusted);

		BRect blockRect;
		blockRect.left = 0.0f;
		blockRect.right = Bounds().Width();
		blockRect.top = startPt.y - 1.0f;
		blockRect.bottom = endPt.y + LineHeight(endPosAdjusted) + 1.0f;

		blockRect.InsetBy(2.0f, 0.0f);

		if (blockRect.Intersects(updateRect)) {
			// A. Disegniamo lo sfondo pieno del riquadro (coprendo l'area del codice)
			SetDrawingMode(B_OP_COPY);
			SetHighColor(blockBgColor);
			FillRoundRect(blockRect, 4.0f, 4.0f);

			// B. Ridisegniamo il testo del codice sopra al riquadro con il colore dedicato
			SetDrawingMode(B_OP_OVER);
			SetHighColor(codeTextColor);
			SetFont(be_fixed_font);

			int32 currentOffset = block->startPos;
			while (currentOffset < block->endPos) {
				BPoint linePt = PointAt(currentOffset);
				
				int32 lineEnd = currentOffset;
				while (lineEnd < block->endPos && ByteAt(lineEnd) != '\n') {
					lineEnd++;
				}

				int32 length = lineEnd - currentOffset;
				if (length > 0) {
					BString lineStr;
					GetText(currentOffset, length, lineStr.LockBuffer(length + 1));
					lineStr.UnlockBuffer();

					DrawString(lineStr.String(), BPoint(linePt.x, linePt.y + LineHeight(currentOffset) - 3.0f));
				}

				currentOffset = lineEnd + 1;
			}
		}
	}

	PopState();
}

status_t
BMarkdownView::SetMarkdown(const BString& markdownText)
{
	return SetMarkdown(markdownText.String());
}

status_t
BMarkdownView::SetMarkdown(const char* markdownText)
{
	SetText("");
	fCodeBlocks.MakeEmpty(true);
	fRawMarkdown.SetTo(markdownText);

	if (markdownText == NULL || strlen(markdownText) == 0)
		return B_OK;

	RenderState state;
	state.view = this;
	
	SetFontAndColor(be_plain_font);
	GetFont(&state.baseFont);
	state.currentFont = state.baseFont;
	
	state.textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
	state.codeColor = (rgb_color){ 200, 40, 40, 255 }; // Usato solo per il codice inline `testo`

	MD_PARSER parser = {
		0,
		MD_FLAG_TABLES,
		_EnterBlockCb,
		_LeaveBlockCb,
		_EnterSpanCb,
		_LeaveSpanCb,
		_TextCb,
		NULL,
		NULL
	};

	int result = md_parse(markdownText, (MD_SIZE)strlen(markdownText), &parser, &state);
	Invalidate();
	return (result == 0) ? B_OK : B_ERROR;
}

void BMarkdownView::InsertRaw(int32 offset, const char* text, int32 length)
{
	fRawMarkdown.Insert(text, length, offset);
	SetMarkdown(fRawMarkdown);
}

void BMarkdownView::InsertRaw(const char* text, int32 length)
{
	InsertRaw(fRawMarkdown.Length(), text, length);
}

void BMarkdownView::InsertRaw(const char* text)
{
	InsertRaw(fRawMarkdown.Length(), text, strlen(text));
}

int32 BMarkdownView::RawTextLength() const
{
	return fRawMarkdown.Length();
}

const char*
BMarkdownView::RawText() const
{
	return fRawMarkdown.String();
}

void
BMarkdownView::_ApplyCurrentStyle(int32 startPos, RenderState& state)
{
	int32 endPos = TextLength();
	if (startPos >= endPos)
		return;

	uint16 face = B_REGULAR_FACE;
	if (state.isBold || state.headingLevel > 0)
		face |= B_BOLD_FACE;
	if (state.isItalic)
		face |= B_ITALIC_FACE;

	if (state.isCode || state.isBlockCode)
		state.currentFont = *be_fixed_font;
	else
		state.currentFont = state.baseFont;

	state.currentFont.SetFace(face);

	if (state.headingLevel > 0) {
		float factor = 1.0f + (0.15f * (7 - std::min(state.headingLevel, (uint32)6)));
		state.currentFont.SetSize(state.baseFont.Size() * factor);
	} else {
		state.currentFont.SetSize(state.baseFont.Size());
	}

	// Selezione del colore del font
	rgb_color colorToApply;
	if (state.isBlockCode) {
		// Nei blocchi usiamo il colore ad alto contrasto per il riquadro invertito
		colorToApply = state.codeColor;
	} else if (state.isCode) {
		// Nel codice inline (`testo`) usiamo una tinta di evidenziazione
		colorToApply = (rgb_color){ 200, 40, 40, 255 };
	} else {
		colorToApply = state.textColor;
	}

	SetFontAndColor(startPos, endPos, &state.currentFont, B_FONT_ALL, &colorToApply);
}

// -----------------------------------------------------------------------------
// Callbacks MD4C
// -----------------------------------------------------------------------------

int
BMarkdownView::_EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);

	switch (type) {
		case MD_BLOCK_H: {
			MD_BLOCK_H_DETAIL* hDetail = static_cast<MD_BLOCK_H_DETAIL*>(detail);
			state->headingLevel = hDetail->level;
			break;
		}
		case MD_BLOCK_CODE:
			state->isBlockCode = true;
			state->view->Insert("\n");
			state->currentBlockStart = state->view->TextLength();
			break;
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);

	switch (type) {
		case MD_BLOCK_H:
			state->headingLevel = 0;
			state->view->Insert("\n\n");
			break;
		case MD_BLOCK_P:
			state->view->Insert("\n\n");
			break;
		case MD_BLOCK_CODE: {
			state->isBlockCode = false;
			int32 blockEnd = state->view->TextLength();
			
			if (state->currentBlockStart != -1 && blockEnd > state->currentBlockStart) {
				CodeBlockRegion* region = new CodeBlockRegion();
				region->startPos = state->currentBlockStart;
				region->endPos = blockEnd;
				state->view->fCodeBlocks.AddItem(region);
			}
			state->currentBlockStart = -1;
			state->view->Insert("\n\n");
			break;
		}
		case MD_BLOCK_LI:
			state->view->Insert("\n");
			break;
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_EnterSpanCb(MD_SPANTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);

	switch (type) {
		case MD_SPAN_STRONG:
			state->isBold = true;
			break;
		case MD_SPAN_EM:
			state->isItalic = true;
			break;
		case MD_SPAN_CODE:
			state->isCode = true;
			break;
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_LeaveSpanCb(MD_SPANTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);

	switch (type) {
		case MD_SPAN_STRONG:
			state->isBold = false;
			break;
		case MD_SPAN_EM:
			state->isItalic = false;
			break;
		case MD_SPAN_CODE:
			state->isCode = false;
			break;
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);
	
	int32 startPos = state->view->TextLength();
	
	BString str(text, size);
	state->view->Insert(str.String());

	state->view->_ApplyCurrentStyle(startPos, *state);

	return 0;
}
