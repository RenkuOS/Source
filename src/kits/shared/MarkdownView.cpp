#include "MarkdownView.h"

#include <InterfaceDefs.h>
#include <TranslationUtils.h>
#include <algorithm>
#include <Cursor.h>
#include <Path.h>
#include <Window.h>
#include <Url.h>
#include <Clipboard.h>
#include <cctype>

#include <cstdio>

BMarkdownView::BMarkdownView(const char* name, uint32 flags)
    :
    BTextView(name, flags),
    fHandCursor(B_CURSOR_ID_FOLLOW_LINK),
    fCodeBlocks(20),
    fTables(10),
    fImages(10),
    fLinks(20),
    fQuotes(20)
{
    _Init();
}


BMarkdownView::BMarkdownView(const char* name, const BFont* font,
    const rgb_color* color, uint32 flags)
    :
    BTextView(name, font, color, flags),
    fHandCursor(B_CURSOR_ID_FOLLOW_LINK),
    fCodeBlocks(20),
    fTables(10),
    fImages(10),
    fLinks(20),
    fQuotes(20)
{
    _Init();
}
BMarkdownView::BMarkdownView(BMessage* archive)
	:
	BTextView(archive),
	fHandCursor(B_CURSOR_ID_FOLLOW_LINK),
	fCodeBlocks(20),
	fTables(10),
	fImages(10),
	fLinks(20),
    fQuotes(20)
{
	_Init();
}


BArchivable*
BMarkdownView::Instantiate(BMessage* archive)
{
	if (validate_instantiation(archive, "BMarkdownView"))
		return new BMarkdownView(archive);

	return NULL;
}


status_t
BMarkdownView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BTextView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("class", "BMarkdownView");

	return status;
}

void
BMarkdownView::_Init()
{
    fRawMarkdown.SetTo("");
    MakeEditable(false);
    MakeSelectable(true);
    SetStylable(true);
}
void
BMarkdownView::_ClearRegions()
{
    // BObjectList<..., true> elimina automaticamente gli oggetti con MakeEmpty()
    fCodeBlocks.MakeEmpty();
    fTables.MakeEmpty();
    fImages.MakeEmpty();
    fLinks.MakeEmpty();

    // Per BList dobbiamo eliminare manualmente gli elementi
    for (int32 i = 0; i < fHorizontalRules.CountItems(); i++) {
        delete static_cast<HorizontalRuleRegion*>(fHorizontalRules.ItemAt(i));
    }
    fHorizontalRules.MakeEmpty();
    
    fQuotes.MakeEmpty();
}


BMarkdownView::~BMarkdownView()
{
    _ClearRegions();
}

void
BMarkdownView::CopyRawMarkdownToClipboard()
{
	if (!fRawMarkdown.IsEmpty() && be_clipboard->Lock()) {
		be_clipboard->Clear();
		BMessage* clip = be_clipboard->Data();
		if (clip != NULL) {
			clip->AddData("text/plain", B_MIME_TYPE,
				fRawMarkdown.String(), fRawMarkdown.Length());
			be_clipboard->Commit();
		}
		be_clipboard->Unlock();
	}
}

void
BMarkdownView::CopyPlainTextToClipboard()
{
	if (fRawMarkdown.IsEmpty())
		return;

	// Depuriamo il Markdown grezzo da tutte le marcature
	BString plainText = _ConvertMarkdownToPlainText(fRawMarkdown);

	if (!plainText.IsEmpty() && be_clipboard->Lock()) {
		be_clipboard->Clear();
		BMessage* clip = be_clipboard->Data();
		if (clip != NULL) {
			clip->AddData("text/plain", B_MIME_TYPE,
				plainText.String(), plainText.Length());
			be_clipboard->Commit();
		}
		be_clipboard->Unlock();
	}
}

BString
BMarkdownView::_ConvertMarkdownToPlainText(const BString& markdown)
{
	BString result;
	int32 len = markdown.Length();
	int32 i = 0;

	bool inCodeBlock = false;

	while (i < len) {
		// 1. Gestione blocchi di codice (```)
		if (markdown.ByteAt(i) == '`' && i + 2 < len 
			&& markdown.ByteAt(i + 1) == '`' && markdown.ByteAt(i + 2) == '`') {
			
			inCodeBlock = !inCodeBlock;
			i += 3;
			// Saltiamo l'eventuale identificatore di linguaggio fino al primo \n
			while (i < len && markdown.ByteAt(i) != '\n') {
				i++;
			}
			if (i < len && markdown.ByteAt(i) == '\n')
				i++;
			continue;
		}

		// Se siamo dentro un blocco di codice, manteniamo il testo intatto
		if (inCodeBlock) {
			result.Append(markdown.ByteAt(i), 1);
			i++;
			continue;
		}

		// 2. Inizio riga: rimuoviamo prefissi di Titoli (#), Citazioni (>), Liste (*, -, 1.)
		if (i == 0 || markdown.ByteAt(i - 1) == '\n') {
			// Rimuoviamo gli '#' dei titoli
			while (i < len && markdown.ByteAt(i) == '#')
				i++;

			// Rimuoviamo gli spazi dopo i titoli
			if (i < len && markdown.ByteAt(i) == ' ' && markdown.ByteAt(i - 1) == '#')
				i++;

			// Rimuoviamo il prefisso citazione '> '
			if (i < len && markdown.ByteAt(i) == '>') {
				i++;
				if (i < len && markdown.ByteAt(i) == ' ')
					i++;
			}

			// Rimuoviamo i punti elenco e liste numerate (* , - , 1. )
			if (i < len && (markdown.ByteAt(i) == '*' || markdown.ByteAt(i) == '-') 
				&& (i + 1 < len && markdown.ByteAt(i + 1) == ' ')) {
				i += 2;
			} else if (i < len && isdigit(markdown.ByteAt(i))) {
				int32 temp = i;
				while (temp < len && isdigit(markdown.ByteAt(temp)))
					temp++;
				if (temp < len && (markdown.ByteAt(temp) == '.' || markdown.ByteAt(temp) == ')')
					&& temp + 1 < len && markdown.ByteAt(temp + 1) == ' ') {
					i = temp + 2;
				}
			}
		}

		if (i >= len)
			break;

		char c = markdown.ByteAt(i);

		// 3. Rimuoviamo marcature inline: Bold/Italic (* o _), Inline Code (`), Strikethrough (~)
		if (c == '*' || c == '_' || c == '`' || c == '~') {
			i++;
			continue;
		}

		// 4. Gestione Link e Immagini: [Testo](url) o ![Alt](url) -> Teniamo solo 'Testo' o 'Alt'
		if (c == '!' && i + 1 < len && markdown.ByteAt(i + 1) == '[') {
			i++; // Saltiamo '!' per trattarlo come link standard
			c = markdown.ByteAt(i);
		}

		if (c == '[') {
			i++;
			// Svuotiamo il testo dell'etichetta prima delle parentesi tonde
			while (i < len && markdown.ByteAt(i) != ']') {
				result.Append(markdown.ByteAt(i), 1);
				i++;
			}
			if (i < len && markdown.ByteAt(i) == ']')
				i++; // Saltiamo ']'

			// Saltiamo l'URL tra parentesi tonde (url)
			if (i < len && markdown.ByteAt(i) == '(') {
				while (i < len && markdown.ByteAt(i) != ')') {
					i++;
				}
				if (i < len && markdown.ByteAt(i) == ')')
					i++; // Saltiamo ')'
			}
			continue;
		}

		// 5. Gestione Tabelle: Rimuoviamo i separatori di colonna '|' e le righe di intestazione '|---|---|'
		if (c == '|') {
			// Se è una riga separatore di tabella (|---|---|), la saltiamo interamente
			int32 nextNL = markdown.FindFirst('\n', i);
			if (nextNL != B_ERROR) {
				BString line;
				markdown.CopyInto(line, i, nextNL - i);
				if (line.FindFirst("---") != B_ERROR) {
					i = nextNL + 1;
					continue;
				}
			}
			// Altrimenti sostituiamo '|' con uno spazio o tabulazione per separare le celle
			result.Append("  ");
			i++;
			continue;
		}

		// Carattere testo normale
		result.Append(c, 1);
		i++;
	}

	return result;
}
void
BMarkdownView::_DrawFormattedSegment(FormattedSegment* seg, BPoint& drawPt, float lineMaxAscent)
{
	if (seg == NULL || seg->text.IsEmpty())
		return;

	BFont font(be_plain_font);
	rgb_color color = ui_color(B_DOCUMENT_TEXT_COLOR);

	uint16 face = font.Face();
	if (seg->isBold)
		face |= B_BOLD_FACE;
	if (seg->isItalic)
		face |= B_ITALIC_FACE;
	if (seg->isLink)
		face |= B_UNDERSCORE_FACE;

	font.SetFace(face);

	if (seg->isCode) {
		font = *be_fixed_font;
		color = (rgb_color){ 200, 40, 40, 255 }; // Rosso per codice inline
	} else if (seg->isLink) {
		color = ui_color(B_LINK_TEXT_COLOR);
	}

	SetFont(&font);
	SetHighColor(color);

	DrawString(seg->text.String(), BPoint(drawPt.x, drawPt.y + lineMaxAscent));
	drawPt.x += font.StringWidth(seg->text.String());
}
void
BMarkdownView::Draw(BRect updateRect)
{
	// 1. BTextView disegna tutto il testo (compreso il testo chiaro del codice)
	// ma lo fa sullo sfondo bianco standard del documento.
	BTextView::Draw(updateRect);

	PushState();
	
	// Rendering delle immagini
	int32 imageCount = fImages.CountItems();
	if (imageCount > 0) {
		for (int32 i = 0; i < imageCount; i++) {
			ImageRegion* img = fImages.ItemAt(i);
			if (img == NULL || img->bitmap == NULL)
				continue;

			BPoint startPt = PointAt(img->startPos);
			BRect bitmapBounds = img->bitmap->Bounds();

			float maxWidth = Bounds().Width() - 20.0f;
			float imgWidth = bitmapBounds.Width();
			float imgHeight = bitmapBounds.Height();

			// Scalatura proporzionale
			if (imgWidth > maxWidth && maxWidth > 0.0f) {
				float scale = maxWidth / imgWidth;
				imgWidth = maxWidth;
				imgHeight *= scale;
			}

			// Posizionamento preciso: 
			// startPt.y della BTextView punta alla top-line della riga.
			// Aggiungiamo un piccolo offset di 2px per centrare visivamente l'immagine nelle righe riservate.
			BRect drawRect(
				startPt.x + 5.0f,
				startPt.y + 2.0f,
				startPt.x + 5.0f + imgWidth,
				startPt.y + 2.0f + imgHeight
			);

			if (drawRect.Intersects(updateRect)) {
				SetDrawingMode(B_OP_ALPHA);
				SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_COMPOSITE);
				DrawBitmap(img->bitmap, img->bitmap->Bounds(), drawRect);
			}
		}
	}
	
	rgb_color docBg = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
	float luminance = (0.299f * docBg.red + 0.587f * docBg.green + 0.114f * docBg.blue);
	
	// if there's no tables just skip this part
	// -------------------------------------------------------------------------
	// RENDERING DELLE TABELLE (Sfondi alternati, Bordi e Testo)
	// -------------------------------------------------------------------------
	int32 tableCount = fTables.CountItems();
	if (tableCount > 0) {
		bool isDark = (luminance < 128.0f);
		rgb_color tableBorderColor  = isDark ? (rgb_color){ 60, 65, 70, 255 } : (rgb_color){ 210, 215, 220, 255 };
		rgb_color headerBgColor      = isDark ? (rgb_color){ 45, 50, 55, 255 } : (rgb_color){ 240, 243, 246, 255 };
		rgb_color altRowBgColor     = isDark ? (rgb_color){ 35, 38, 42, 255 } : (rgb_color){ 248, 249, 250, 255 };
		rgb_color normalRowBgColor  = isDark ? (rgb_color){ 28, 30, 33, 255 } : (rgb_color){ 255, 255, 255, 255 };

		font_height fh;
		be_plain_font->GetHeight(&fh);
		float fontLineHeight = fh.ascent + fh.descent + fh.leading;

		for (int32 t = 0; t < tableCount; t++) {
			TableRegion* table = fTables.ItemAt(t);
			if (table == NULL || table->rows.CountItems() == 0)
				continue;

			int32 rowCount = table->rows.CountItems();
			TableRowRegion* firstRow = table->rows.ItemAt(0);
			TableRowRegion* lastRow  = table->rows.ItemAt(rowCount - 1);

			BPoint startPt = PointAt(firstRow->startPos);
			int32 lastPosAdjusted = std::max(lastRow->startPos, lastRow->endPos - 1);
			BPoint endPt = PointAt(lastPosAdjusted);

			// 1. Spazio superiore dedicato per il tasto "Copy table" (20px)
			float headerBarHeight = 20.0f;

			BRect totalTableRect;
			totalTableRect.left = 10.0f;
			totalTableRect.right = Bounds().Width() - 10.0f;
			totalTableRect.top = startPt.y - 4.0f;
			totalTableRect.bottom = endPt.y + LineHeight(lastPosAdjusted) + 4.0f;

			BRect outerRect = totalTableRect;
			outerRect.top -= headerBarHeight; // Fa spazio per la label del copia

			if (!outerRect.Intersects(updateRect))
				continue;

			// 2. ZEBRA STRIPING DELLE RIGHE (Sfondo e linee orizzontali)
			for (int32 r = 0; r < rowCount; r++) {
				TableRowRegion* row = table->rows.ItemAt(r);
				BPoint rStartPt = PointAt(row->startPos);
				int32 rEndAdjusted = std::max(row->startPos, row->endPos - 1);

				float rHeight = LineHeight(rEndAdjusted);
				if (rHeight < fontLineHeight)
					rHeight = fontLineHeight;

				BRect rowRect;
				rowRect.left = totalTableRect.left;
				rowRect.right = totalTableRect.right;
				rowRect.top = rStartPt.y - 2.0f;
				rowRect.bottom = rowRect.top + rHeight + 4.0f;

				rgb_color rowBg;
				if (row->isHeader)
					rowBg = headerBgColor;
				else if (r % 2 == 1)
					rowBg = altRowBgColor;
				else
					rowBg = normalRowBgColor;

				SetDrawingMode(B_OP_COPY);
				SetHighColor(rowBg);
				FillRect(rowRect);

				// Linea orizzontale sotto ogni riga
				SetHighColor(tableBorderColor);
				StrokeLine(BPoint(rowRect.left, rowRect.bottom), BPoint(rowRect.right, rowRect.bottom));
			}

			// 3. LINEE VERTICALI DIVISORIE TRA LE COLONNE
			int32 maxCols = 0;
			for (int32 r = 0; r < rowCount; r++) {
				TableRowRegion* row = table->rows.ItemAt(r);
				if (row != NULL && row->cells.CountItems() > maxCols)
					maxCols = row->cells.CountItems();
			}

			if (maxCols > 1) {
				float colWidth = totalTableRect.Width() / (float)maxCols;
				SetHighColor(tableBorderColor);

				for (int32 c = 1; c < maxCols; c++) {
					float xLine = totalTableRect.left + (c * colWidth);
					StrokeLine(
						BPoint(xLine, totalTableRect.top),
						BPoint(xLine, totalTableRect.bottom)
					);
				}
			}

			// 4. BORDO ESTERNO ARROTONDATO DELL'INTERA TABELLA
			SetHighColor(tableBorderColor);
			StrokeRoundRect(totalTableRect, 4.0f, 4.0f);

			// 5. PULSANTE / LABEL "Copy table" IN ALTO A DESTRA (Sopra la tabella, visibile ed intero)
			SetDrawingMode(B_OP_OVER);
			SetFont(be_plain_font);
			SetHighColor(isDark ? (rgb_color){ 180, 185, 190, 255 } : (rgb_color){ 100, 105, 110, 255 });

			const char* copyTableStr = "📑 Copy table";
			float copyWidth = StringWidth(copyTableStr);

			table->copyRect.Set(
				totalTableRect.right - copyWidth - 8.0f,
				totalTableRect.top - headerBarHeight + 2.0f,
				totalTableRect.right,
				totalTableRect.top - 2.0f
			);

			DrawString(copyTableStr, BPoint(table->copyRect.left + 2.0f, totalTableRect.top - 5.0f));

			// 6. RIDISEGNO PRECISO DEL TESTO DELLE CELLE ALLINEATO AL CENTRO VERTICALE
			float colWidth = (maxCols > 0) ? (totalTableRect.Width() / (float)maxCols) : totalTableRect.Width();

			for (int32 r = 0; r < rowCount; r++) {
				TableRowRegion* row = table->rows.ItemAt(r);
				if (row == NULL) continue;

				BPoint rStartPt = PointAt(row->startPos);
				int32 rEndAdjusted = std::max(row->startPos, row->endPos - 1);
				float rHeight = LineHeight(rEndAdjusted);
				if (rHeight < fontLineHeight)
					rHeight = fontLineHeight;

				// Baseline calcolata precisamente per centrare il testo nella riga
				float textY = rStartPt.y + (rHeight - fontLineHeight) / 2.0f;

				int32 cellCount = row->cells.CountItems();
				for (int32 c = 0; c < cellCount; c++) {
					TableCellRegion* cell = row->cells.ItemAt(c);
					if (cell == NULL || cell->segments.IsEmpty())
						continue;

					float cellX = totalTableRect.left + (c * colWidth) + 8.0f;
					BPoint drawPt(cellX, textY);

					int32 segCount = cell->segments.CountItems();
					for (int32 s = 0; s < segCount; s++) {
						FormattedSegment* seg = cell->segments.ItemAt(s);
						_DrawFormattedSegment(seg, drawPt, fh.ascent);
					}
				}
			}
		}
	}
	
	// if there's no codeblocks just exit!
	int32 count = fCodeBlocks.CountItems();
	if (count > 0) {
		rgb_color blockBgColor  = (luminance >= 128.0f) ? (rgb_color){ 35, 38, 41, 255 } : (rgb_color){ 245, 242, 220, 255 };
		rgb_color headerBgColor = (luminance >= 128.0f) ? (rgb_color){ 28, 30, 33, 255 } : (rgb_color){ 230, 227, 205, 255 };
		rgb_color codeTextColor = (luminance >= 128.0f) ? (rgb_color){ 235, 238, 242, 255 } : (rgb_color){ 25, 25, 25, 255 };
		rgb_color borderColor   = (luminance >= 128.0f) ? (rgb_color){ 60, 65, 70, 255 } : (rgb_color){ 210, 205, 180, 255 };
		
		font_height fh;
		be_fixed_font->GetHeight(&fh);
		float lineHeight = fh.ascent + fh.descent + fh.leading;

		for (int32 i = 0; i < count; i++) {
			CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
			if (block == NULL || block->codeText.IsEmpty())
				continue;

			int32 totalLines = 0;
			int32 strPos = 0;
			int32 codeLen = block->codeText.Length();
			while (strPos < codeLen) {
				totalLines++;
				int32 lineEnd = block->codeText.FindFirst('\n', strPos);
				if (lineEnd == B_ERROR)
					break;
				strPos = lineEnd + 1;
			}
			if (totalLines < 1) totalLines = 1;

			// 2. Calcoliamo la coordinata di partenza top basandoci su startPos
			BPoint startPt = PointAt(block->startPos);
			float headerHeight = 22.0f;
			float codeHeight = totalLines * lineHeight;
			float totalBlockHeight = headerHeight + codeHeight + 12.0f; // 12px di padding globale

			BRect blockRect;
			blockRect.left = 2.0f;
			blockRect.right = Bounds().Width() - 2.0f;
			blockRect.top = startPt.y - headerHeight - 4.0f; // Fa salire il riquadro per racchiudere l'header
			blockRect.bottom = blockRect.top + totalBlockHeight;

			if (blockRect.Intersects(updateRect)) {
				// A. Sfondo del riquadro principale
				SetDrawingMode(B_OP_COPY);
				SetHighColor(blockBgColor);
				FillRoundRect(blockRect, 4.0f, 4.0f);

				// B. Barra d'intestazione superiore (Header)
				BRect headerRect(blockRect.left, blockRect.top, blockRect.right, blockRect.top + headerHeight);
				SetHighColor(headerBgColor);
				FillRoundRect(headerRect, 4.0f, 4.0f);

				// Linea di separazione sotto l'header
				SetHighColor(borderColor);
				StrokeLine(BPoint(headerRect.left, headerRect.bottom), BPoint(headerRect.right, headerRect.bottom));

				// Bordo esterno arrotondato
				StrokeRoundRect(blockRect, 4.0f, 4.0f);

				// C. Pulsante "Copia"
				SetDrawingMode(B_OP_OVER);
				SetHighColor(codeTextColor);
				SetFont(be_plain_font);

				const char* copyStr = "📑 Copy";
				float copyWidth = StringWidth(copyStr);

				block->copyRect.Set(
					headerRect.right - copyWidth - 12.0f,
					headerRect.top + 2.0f,
					headerRect.right - 4.0f,
					headerRect.bottom - 2.0f
				);

				DrawString(copyStr, BPoint(block->copyRect.left + 2.0f, headerRect.top + 15.0f));

				// D. Testo del codice sorgente
				SetFont(be_fixed_font);
				const float codeLeftPadding = 10.0f;
				float currentY = headerRect.bottom + 6.0f;

				strPos = 0;
				while (strPos < codeLen) {
					int32 lineEnd = block->codeText.FindFirst('\n', strPos);
					if (lineEnd == B_ERROR)
						lineEnd = codeLen;

					BString lineStr;
					block->codeText.CopyInto(lineStr, strPos, lineEnd - strPos);

					DrawString(lineStr.String(), BPoint(blockRect.left + codeLeftPadding, currentY + fh.ascent));

					currentY += lineHeight;
					strPos = lineEnd + 1;
				}
			}
		}
	}
	
	// -------------------------------------------------------------------------
	// RENDERING CITAZIONI CON SEGMENTI FORMATTATI (Bold, Italic, Link, Inline Code)
	// -------------------------------------------------------------------------
	int32 quoteCount = fQuotes.CountItems();
	if (quoteCount > 0) {
		rgb_color panelColor = ui_color(B_PANEL_BACKGROUND_COLOR);
		rgb_color bgColor    = tint_color(panelColor, B_DARKEN_1_TINT);
		rgb_color barColor   = tint_color(panelColor, B_DARKEN_3_TINT);

		font_height fh;
		be_plain_font->GetHeight(&fh);
		float lineHeight = fh.ascent + fh.descent + fh.leading;

		for (int32 i = 0; i < quoteCount; i++) {
			QuoteRegion* quote = fQuotes.ItemAt(i);
			if (quote == NULL || quote->segments.IsEmpty())
				continue;

			// 1. Calcoliamo le righe totali contando i '\n' nei segmenti
			int32 totalLines = 1;
			int32 segCount = quote->segments.CountItems();
			for (int32 s = 0; s < segCount; s++) {
				FormattedSegment* seg = quote->segments.ItemAt(s);
				if (seg != NULL) {
					int32 pos = 0;
					while ((pos = seg->text.FindFirst('\n', pos)) != B_ERROR) {
						totalLines++;
						pos++;
					}
				}
			}

			BPoint startPt = PointAt(quote->startPos);
			float headerPadding = 18.0f; // Spazio per il pulsante copia
			float contentHeight = (totalLines * lineHeight) + 8.0f;

			BRect quoteRect;
			quoteRect.left = 2.0f;
			quoteRect.right = Bounds().Width() - 2.0f;
			quoteRect.top = startPt.y - headerPadding;
			quoteRect.bottom = quoteRect.top + headerPadding + contentHeight;

			if (quoteRect.Intersects(updateRect)) {
				// A. Sfondo pieno della citazione
				SetDrawingMode(B_OP_COPY);
				SetHighColor(bgColor);
				FillRect(quoteRect);

				// B. Barra d'accento verticale a sinistra (spessa 4px)
				BRect barRect(quoteRect.left, quoteRect.top, quoteRect.left + 4.0f, quoteRect.bottom);
				SetHighColor(barColor);
				FillRect(barRect);

				// C. Pulsante "📑 Copy quote" in alto a destra
				SetDrawingMode(B_OP_OVER);
				SetFont(be_plain_font);
				SetHighColor(ui_color(B_DOCUMENT_BACKGROUND_COLOR));

				const char* copyQuoteStr = "📑 Copy quote";
				float copyWidth = StringWidth(copyQuoteStr);

				quote->copyRect.Set(
					quoteRect.right - copyWidth - 12.0f,
					quoteRect.top + 2.0f,
					quoteRect.right - 2.0f,
					quoteRect.top + 18.0f
				);

				DrawString(copyQuoteStr, BPoint(quote->copyRect.left + 2.0f, quoteRect.top + 13.0f));

				// D. Disegno dei segmenti formattati riga per riga
				const float textLeftPadding = 14.0f;
				BPoint drawPt(quoteRect.left + textLeftPadding, quoteRect.top + headerPadding + 2.0f);

				for (int32 s = 0; s < segCount; s++) {
					FormattedSegment* seg = quote->segments.ItemAt(s);
					if (seg == NULL || seg->text.IsEmpty())
						continue;

					// Se il segmento contiene dei '\n', lo spezziamo e andiamo a capo
					int32 strPos = 0;
					int32 textLen = seg->text.Length();

					while (strPos < textLen) {
						int32 lineEnd = seg->text.FindFirst('\n', strPos);
						if (lineEnd == B_ERROR) {
							// Disegniamo il resto del segmento sulla riga corrente
							BString subStr;
							seg->text.CopyInto(subStr, strPos, textLen - strPos);
							
							FormattedSegment subSeg = *seg;
							subSeg.text = subStr;
							_DrawFormattedSegment(&subSeg, drawPt, fh.ascent);
							break;
						} else {
							// Disegniamo fino al '\n' e andiamo a capo
							if (lineEnd > strPos) {
								BString subStr;
								seg->text.CopyInto(subStr, strPos, lineEnd - strPos);
								
								FormattedSegment subSeg = *seg;
								subSeg.text = subStr;
								_DrawFormattedSegment(&subSeg, drawPt, fh.ascent);
							}

							// Reset coordinata X e avanzamento Y
							drawPt.x = quoteRect.left + textLeftPadding;
							drawPt.y += lineHeight;
							strPos = lineEnd + 1;
						}
					}
				}
			}
		}
	}
	// rendering divisori
	int32 hrCount = fHorizontalRules.CountItems();
    if (hrCount > 0) {
    	// Colore della linea: un grigio discreto di sistema
        rgb_color dividerColor = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), B_DARKEN_2_TINT);
        SetHighColor(dividerColor);
        SetPenSize(1.0f);

        float viewWidth = Bounds().Width();
        float leftMargin = 10.0f;
        float rightMargin = viewWidth - 10.0f;

        font_height fh;
        be_plain_font->GetHeight(&fh);
        float lineHeight = fh.ascent + fh.descent + fh.leading;

        for (int32 i = 0; i < hrCount; i++) {
            HorizontalRuleRegion* hr = static_cast<HorizontalRuleRegion*>(fHorizontalRules.ItemAt(i));
            if (hr == NULL || hr->pos < 0 || hr->pos > TextLength())
                continue;

            // Coordinate visive dell'offset di ancoraggio
            BPoint pt = PointAt(hr->pos);

            // Calcoliamo la Y centrata nel gap del newline
            float y = pt.y + (lineHeight / 2.0f);

            BRect hrRect(leftMargin, y - 1.0f, rightMargin, y + 1.0f);
            if (hrRect.Intersects(updateRect)) {
                StrokeLine(BPoint(leftMargin, y), BPoint(rightMargin, y));
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
	_ClearRegions();
	
	if (markdownText == NULL || strlen(markdownText) == 0)
		return B_BAD_VALUE;

	fRawMarkdown.SetTo(markdownText);

	RenderState state;
	state.view = this;
	
	SetFontAndColor(be_plain_font, B_FONT_ALL);
	state.currentFont = *be_plain_font;
	
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
/*
void
BMarkdownView::AttachedToWindow()
{
	BTextView::AttachedToWindow();

	// Ora che la vista è agganciata alla finestra, Bounds().Width() è valido!
	// Se ci sono immagini, rieseguiamo il parsing per calcolare le righe esatte
	if (fImages.CountItems() > 0 && !fRawMarkdown.IsEmpty()) {
		SetMarkdown(fRawMarkdown);
	}
}*/

void
BMarkdownView::FrameResized(float width, float height)
{
	BTextView::FrameResized(width, height);

	if (fImages.CountItems() > 0 && !fRawMarkdown.IsEmpty()) {
		SetMarkdown(fRawMarkdown);
		MakeFocus(IsFocus());
		Invalidate();
	}
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
	
	if (state.isCode || state.isBlockCode || state.isTable) {
		state.currentFont = *be_fixed_font;
	} else if (state.isBold || state.headingLevel > 0) {
		state.currentFont = *be_bold_font;
	} else {
		state.currentFont = *be_plain_font;
	}

	uint16 face = state.currentFont.Face();
	if (state.isItalic)
		face |= B_ITALIC_FACE;
	if (state.isLink)
		face |= B_UNDERSCORE_FACE; // Sottolineato per i collegamenti

	state.currentFont.SetFace(face);

	if (state.headingLevel > 0) {
		float factor = 1.0f + (0.15f * (7 - std::min(state.headingLevel, (uint32)6)));
		state.currentFont.SetSize(be_plain_font->Size() * factor);
	} else {
		state.currentFont.SetSize(be_plain_font->Size());
	}

	// Selezione del colore del font
	rgb_color colorToApply;
	if (state.isLink) {
		// Blu classico o colore di sistema per i link
		colorToApply = ui_color(B_LINK_TEXT_COLOR);
	} else if (state.isBlockCode) {
		// Nei blocchi usiamo il colore ad alto contrasto per il riquadro invertito
		colorToApply = state.codeColor;
	} else if (state.isCode) {
		// Nel codice inline (`testo`) usiamo una tinta di evidenziazione
		colorToApply = (rgb_color){ 200, 40, 40, 255 };
	} else {
		colorToApply = state.textColor;
	}
	
	SetFontAndColor(startPos, endPos, &state.currentFont, B_FONT_ALL, &colorToApply); //
}

// -----------------------------------------------------------------------------
// Callbacks MD4C
// -----------------------------------------------------------------------------
int
BMarkdownView::_EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);
	if (state == NULL || state->view == NULL)
		return 0;

	switch (type) {
		case MD_BLOCK_H: {
			MD_BLOCK_H_DETAIL* hDetail = static_cast<MD_BLOCK_H_DETAIL*>(detail);
			state->headingLevel = hDetail->level;
			break;
		}
		case MD_BLOCK_CODE:
		{
			//state->isBlockCode = true;
			state->view->Insert("\n"); // Riga riservata per l'header della toolbar
			CodeBlockRegion* region = new CodeBlockRegion();
			region->startPos = state->view->TextLength();
			region->endPos = -1;
			state->currentCodeBlock = region;
			state->blockStack.push(BLOCK_CODE);
			break;
		}
		case MD_BLOCK_TABLE:
			state->isTable = true;
			state->view->Insert("\n");// Spazio extra riservato per il pulsante copia sopra la tabella
			state->currentTable = new TableRegion();
			state->currentTable->startPos = state->view->TextLength();
			break;

		case MD_BLOCK_THEAD:
			state->isHeaderRow = true;
			break;

		case MD_BLOCK_TR: {
			if (state->currentTable != NULL) {
				TableRowRegion* row = new TableRowRegion();
				row->startPos = state->view->TextLength();
				row->isHeader = state->isHeaderRow;
				state->currentTable->rows.AddItem(row);
				state->currentColIndex = 0;
			}
			break;
		}
		case MD_BLOCK_TH:
			//state->isBold = true;
			// fall-through intenzionale verso MD_BLOCK_TD
		case MD_BLOCK_TD: {
			if (state->currentTable != NULL && state->currentTable->rows.CountItems() > 0) {
				TableRowRegion* row = state->currentTable->rows.LastItem();

				// Inseriamo un tab per separare fisicamente le colonne nel buffer di BTextView
				//if (state->currentColIndex > 0) {
				//	state->view->Insert("\t");
				//}

				TableCellRegion* cell = new TableCellRegion();
				cell->startPos = state->view->TextLength();
				cell->colIndex = state->currentColIndex;
				row->cells.AddItem(cell);
				state->currentCell = cell;
				state->blockStack.push(BLOCK_TABLE_CELL);
			}
			break;
		}
		case MD_BLOCK_UL: {
			state->listDepth++;
			state->isOrderedList = false;
			break;
		}
		case MD_BLOCK_OL: {
			MD_BLOCK_OL_DETAIL* olDetail = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
			state->listDepth++;
			state->isOrderedList = true;
			state->olItemNumber = (olDetail != NULL) ? olDetail->start : 1;
			break;
		}
		case MD_BLOCK_LI: {
			int32 startOffset = state->view->TextLength();

			for (int32 i = 0; i < state->listDepth - 1; i++) {
				state->view->Insert("    ");
			}

			if (state->isOrderedList) {
				BString numStr;
				numStr.SetToFormat("%" B_PRId32 ". ", state->olItemNumber++);
				state->view->Insert(numStr.String());
			} else {
				state->view->Insert("\xE2\x80\xA2 ");
			}

			int32 endOffset = state->view->TextLength();

			rgb_color textColor = state->textColor;
			state->view->SetFontAndColor(startOffset, endOffset,
				be_plain_font, B_FONT_ALL, &textColor);
			break;
		}
		case MD_BLOCK_HR: {
			state->view->Insert("\n");

			HorizontalRuleRegion* hr = new HorizontalRuleRegion();
			hr->pos = state->view->TextLength();
			state->view->fHorizontalRules.AddItem(hr);

			state->view->Insert("\n");
			break;
		}
		case MD_BLOCK_QUOTE: {
			// Riserviamo uno spazio iniziale per il pulsante "Copy quote" in alto
			state->view->Insert("\n");
			QuoteRegion* quote = new QuoteRegion();
			quote->startPos = state->view->TextLength();
			quote->endPos = -1;

			state->currentQuote = quote;
			//state->isQuote = true;
			state->blockStack.push(BLOCK_QUOTE);
			break;
		}
		case MD_BLOCK_P: {
			if (state->isQuote) {
				state->view->Insert("\t");
			}
			break;
		}
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);
	if (state == NULL || state->view == NULL)
		return 0;

	switch (type) {
		case MD_BLOCK_H:
			state->headingLevel = 0;
			state->view->Insert("\n\n");
			break;

		case MD_BLOCK_P:
			state->view->Insert("\n");
			break;

		case MD_BLOCK_CODE: {
			state->isBlockCode = false;
			if (state->currentCodeBlock != NULL) {
				state->currentCodeBlock->endPos = state->view->TextLength();
				state->view->fCodeBlocks.AddItem(state->currentCodeBlock);
				state->currentCodeBlock = NULL;
			}
			if (state->CurrentBlock() == BLOCK_CODE)
				state->blockStack.pop();
			state->view->Insert("\n");
			break;
		}

		case MD_BLOCK_LI:
			state->view->Insert("\n");
			break;

		case MD_BLOCK_TABLE:
			if (state->currentTable != NULL) {
				state->currentTable->endPos = state->view->TextLength();
				state->view->fTables.AddItem(state->currentTable);
				state->currentTable = NULL;
			}
			state->isTable = false;
			state->view->Insert("\n");
			break;

		case MD_BLOCK_THEAD:
			state->isHeaderRow = false;
			break;

		case MD_BLOCK_TH:
			// state->isBold = false;
			// fall-through intenzionale verso MD_BLOCK_TD
		case MD_BLOCK_TD:
			if (state->currentCell != NULL) {
				state->currentCell->endPos = state->view->TextLength();
				state->currentCell = NULL;
			}
			if (state->CurrentBlock() == BLOCK_TABLE_CELL)
				state->blockStack.pop();
			state->currentColIndex++;
			break;

		case MD_BLOCK_TR: {
			if (state->currentTable != NULL && state->currentTable->rows.CountItems() > 0) {
				TableRowRegion* row = state->currentTable->rows.LastItem();
				row->endPos = state->view->TextLength();
				state->view->Insert("\n");
			}
			break;
		}

		case MD_BLOCK_UL:
		case MD_BLOCK_OL: {
			if (state->listDepth > 0)
				state->listDepth--;
			break;
		}

		case MD_BLOCK_QUOTE: {
			if (state->currentQuote != NULL) {
				state->currentQuote->endPos = state->view->TextLength();
				state->view->fQuotes.AddItem(state->currentQuote);
				state->currentQuote = NULL;
			}
			state->isQuote = false;
			state->view->Insert("\n"); // A capo dopo la citazione
			break;
		}
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
		case MD_SPAN_A: {
			MD_SPAN_A_DETAIL* aDetail = static_cast<MD_SPAN_A_DETAIL*>(detail);
			state->isLink = true;

			LinkRegion* link = new LinkRegion();
			link->startPos = state->view->TextLength();

			if (aDetail->href.text != NULL && aDetail->href.size > 0)
				link->url.SetTo(aDetail->href.text, aDetail->href.size);

			state->currentLink = link;
			break;
		}
		case MD_SPAN_STRONG:
			state->isBold = true;
			break;
		case MD_SPAN_EM:
			state->isItalic = true;
			break;
		case MD_SPAN_CODE:
			state->isCode = true;
			break;
		case MD_SPAN_IMG: {
			MD_SPAN_IMG_DETAIL* imgDetail = static_cast<MD_SPAN_IMG_DETAIL*>(detail);

	ImageRegion* imgRegion = new ImageRegion();
	// Nota: non inseriamo inserimenti di testo qui!

	if (imgDetail->src.text != NULL && imgDetail->src.size > 0)
		imgRegion->src.SetTo(imgDetail->src.text, imgDetail->src.size);

	state->view->_LoadImageForRegion(imgRegion);

	state->currentImage = imgRegion;
	state->currentImageAlt.SetTo("");
	state->isImage = true;
	break;
		}
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
		case MD_SPAN_A: {
			if (state->currentLink != NULL) {
				state->currentLink->endPos = state->view->TextLength();
				//state->view->fLinks.AddItem(state->currentLink);
				if (state->currentLink->endPos > state->currentLink->startPos) {
					state->view->fLinks.AddItem(state->currentLink);
				} else {
					delete state->currentLink;
				}
				state->currentLink = NULL;
			}
			state->isLink = false; // Disattiva lo stato link!
			break;
		}
		case MD_SPAN_STRONG:
			state->isBold = false;
			break;
		case MD_SPAN_EM:
			state->isItalic = false;
			break;
		case MD_SPAN_CODE:
			state->isCode = false;
			break;
		case MD_SPAN_IMG: {
	state->isImage = false;

	if (state->currentImage != NULL) {
		state->currentImage->alt = state->currentImageAlt;

		if (state->currentImage->bitmap != NULL && state->currentImage->bitmap->IsValid()) {
			// 1. Dimensioni e scalatura
			BRect bitmapBounds = state->currentImage->bitmap->Bounds();
			float nativeWidth = bitmapBounds.Width();
			float nativeHeight = bitmapBounds.Height();

			float viewWidth = state->view->Bounds().Width();
			float maxWidth = viewWidth - 20.0f;

			float renderedHeight = nativeHeight;
			if (maxWidth > 0.0f && nativeWidth > maxWidth) {
				float scale = maxWidth / nativeWidth;
				renderedHeight = nativeHeight * scale;
			}

			// 2. Misuriamo l'altezza esatta di UNA riga vuota nella BTextView
			font_height fh;
			be_plain_font->GetHeight(&fh);
			float lineHeight = fh.ascent + fh.descent + fh.leading;
			if (lineHeight < 1.0f)
				lineHeight = 16.0f;

			// 3. Calcolo dei newlines corretti:
			// Usiamo floorf e sottraiamo 1 per compensare il \n automatico del paragrafo Markdown
			int32 newLinesNeeded = (int32)floorf(renderedHeight / lineHeight);
			if (newLinesNeeded > 2) {
				newLinesNeeded -= 2; // Togliamo i 2 newlines di troppo (margine/paragrafo)
			} else if (newLinesNeeded < 1) {
				newLinesNeeded = 1;
			}

			int32 imageStartPos = state->view->TextLength();

			for (int32 i = 0; i < newLinesNeeded; i++) {
				state->view->Insert("\n");
			}

			int32 imageEndPos = state->view->TextLength();

			// Forziamo il font plain su tutta la spaziatura riservata
			rgb_color textColor = state->textColor;
			state->view->SetFontAndColor(imageStartPos, imageEndPos,
				be_plain_font, B_FONT_ALL, &textColor);

			state->currentImage->startPos = imageStartPos;
			state->currentImage->endPos = imageEndPos;
			state->view->fImages.AddItem(state->currentImage);
		} else {
			if (!state->currentImage->alt.IsEmpty()) {
				BString altFallback;
				altFallback.SetToFormat("[%s]", state->currentImage->alt.String());
				state->view->Insert(altFallback.String());
			}
			state->view->Insert("\n");
			delete state->currentImage;
		}
		state->currentImage = NULL;
	}
	break;
}
		default:
			break;
	}
	return 0;
}

int
BMarkdownView::_TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata)
{
	RenderState* state = static_cast<RenderState*>(userdata);
	if (state == NULL || state->view == NULL || text == NULL || size == 0)
		return 0;

	ContainerBlockType currentBlock = state->CurrentBlock();

	// Se siamo all'interno di uno span immagine, usiamo BString(text, size)
	// per evitare problemi di puntatori non terminati da '\0'
	if (state->isImage) {
		if (text != NULL && size > 0)
			state->currentImageAlt.Append(text, size);
		return 0; // NON scriviamo l'alt text nel documento visivo!
	}
	
	if (currentBlock == BLOCK_CODE) {
		for (MD_SIZE i = 0; i < size; i++) {
			if (text[i] == '\n')
				state->view->Insert("\n");
		}
		if (state->currentCodeBlock != NULL)
			state->currentCodeBlock->codeText.Append(text, size);
		return 0;
	}
	
	if (currentBlock == BLOCK_QUOTE || currentBlock == BLOCK_TABLE_CELL) {
		for (MD_SIZE i = 0; i < size; i++) {
			if (text[i] == '\n')
				state->view->Insert("\n");
		}
		state->view->_AppendFormattedText(state, text, size);
		return 0;
	}
	
	int32 startPos = state->view->TextLength();
	
	BString str(text, size);
	state->view->Insert(str.String());

	state->view->_ApplyCurrentStyle(startPos, *state);

	return 0;
}
void
BMarkdownView::_LoadImageForRegion(ImageRegion* region)
{
	if (region == NULL || region->src.IsEmpty())
		return;
		
	// Caricamento da file locale (es. /boot/home/images/photo.png o relativo)
	region->bitmap = BTranslationUtils::GetBitmap(region->src.String());

	// Se il percorso è relativo o l'immagine non è stata trovata directly
	if (region->bitmap == NULL && region->src.ByteAt(0) != '/') {
		// Tentativo c	on percorso assoluto o relativo alla directory corrente
		BPath path(region->src.String());
		region->bitmap = BTranslationUtils::GetBitmap(path.Path());
	}
}
LinkRegion*
BMarkdownView::_LinkAt(BPoint point) const
{
	// Convertiamo le coordinate visive del punto nell'offset di testo della BTextView
	int32 offset = OffsetAt(point);
	if (offset < 0 || offset >= TextLength())
		return NULL;

	int32 linkCount = fLinks.CountItems();
	for (int32 i = 0; i < linkCount; i++) {
		LinkRegion* link = fLinks.ItemAt(i);
		if (link != NULL && offset >= link->startPos && offset < link->endPos) {
			return link;
		}
	}

	return NULL;
}

void
BMarkdownView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
	BTextView::MouseMoved(where, transit, dragMessage);

	bool overInteractiveElement = false;

	// 1. Check passaggio su pulsante "Copia" nei CodeBlock
	int32 codeCount = fCodeBlocks.CountItems();
	for (int32 i = 0; i < codeCount; i++) {
		CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
		if (block != NULL && block->copyRect.Contains(where)) {
			overInteractiveElement = true;
			break;
		}
	}
	// 2. Check passaggio su pulsante "Copia" nelle Tabelle
	if (!overInteractiveElement) {
		int32 tableCount = fTables.CountItems();
		for (int32 t = 0; t < tableCount; t++) {
			TableRegion* table = fTables.ItemAt(t);
			if (table != NULL && table->copyRect.Contains(where)) {
				overInteractiveElement = true;
				break;
			}
		}
	}
	// 3. Check passaggio su pulsante "Copia" nelle Citazioni (Quotes)
	if (!overInteractiveElement) {
		int32 quoteCount = fQuotes.CountItems();
		for (int32 q = 0; q < quoteCount; q++) {
			QuoteRegion* quote = fQuotes.ItemAt(q);
			if (quote != NULL && quote->copyRect.Contains(where)) {
				overInteractiveElement = true;
				break;
			}
		}
	}

	// 4. Check passaggio su un Link
	if (!overInteractiveElement && _LinkAt(where) != NULL) {
		overInteractiveElement = true;
	}

	// Gestione dinamica del cursore
	if (overInteractiveElement) {
		SetViewCursor(&fHandCursor);
	} else if (transit == B_INSIDE_VIEW || transit == B_ENTERED_VIEW) {
		BCursor iBeamCursor(B_CURSOR_ID_I_BEAM);
		SetViewCursor(&iBeamCursor);
	}
}
void
BMarkdownView::MouseDown(BPoint where)
{
	
	int32 buttons = 0;
	if (Window() != NULL && Window()->CurrentMessage() != NULL)
		Window()->CurrentMessage()->FindInt32("buttons", &buttons);

	if (buttons == B_PRIMARY_MOUSE_BUTTON) {
		int32 codeCount = fCodeBlocks.CountItems();
		for (int32 i = 0; i < codeCount; i++) {
			CodeBlockRegion* block = fCodeBlocks.ItemAt(i);
			if (block != NULL && block->copyRect.Contains(where)) {
				if (be_clipboard->Lock()) {
					be_clipboard->Clear();
					BMessage* clip = be_clipboard->Data();
					if (clip != NULL) {
						clip->AddData("text/plain", B_MIME_TYPE,
							block->codeText.String(), block->codeText.Length());
						be_clipboard->Commit();
					}
					be_clipboard->Unlock();
				}
				return; // Gestito!
			}
		}

		// 2. Controllo click sul pulsante "Copia" delle Tabelle
		int32 tableCount = fTables.CountItems();
		for (int32 t = 0; t < tableCount; t++) {
			TableRegion* table = fTables.ItemAt(t);
			if (table != NULL && table->copyRect.Contains(where)) {
				BString tableText = table->ToText();
				if (!tableText.IsEmpty() && be_clipboard->Lock()) {
					be_clipboard->Clear();
					BMessage* clip = be_clipboard->Data();
					if (clip != NULL) {
						clip->AddData("text/plain", B_MIME_TYPE,
							tableText.String(), tableText.Length());
						be_clipboard->Commit();
					}
					be_clipboard->Unlock();
				}
				return; // Gestito!
			}
		}
		
		int32 quoteCount = fQuotes.CountItems();
		for (int32 q = 0; q < quoteCount; q++) {
			QuoteRegion* quote = fQuotes.ItemAt(q);
			if (quote != NULL && quote->copyRect.Contains(where)) {
				if (!quote->ToText().IsEmpty() && be_clipboard->Lock()) {
					be_clipboard->Clear();
					BMessage* clip = be_clipboard->Data();
					if (clip != NULL) {
						BString qText = quote->ToText();
						clip->AddData("text/plain", B_MIME_TYPE, qText.String(), qText.Length());
						be_clipboard->Commit();
					}
					be_clipboard->Unlock();
				}
				return; // Gestito!
			}
		}
		
		LinkRegion* link = _LinkAt(where);
		if (link != NULL && !link->url.IsEmpty()) {
			// Usiamo la classe nativa BUrl
			BUrl url(link->url.String(),true);

			if (url.IsValid()) {
				// Metodo nativo reale di BUrl per aprire il link con l'app di sistema
				status_t err = url.OpenWithPreferredApplication();
				if (err == B_OK) {
					return; // Gestito con successo!
				}
			}
		}
	}

	BTextView::MouseDown(where);
}
void
BMarkdownView::_AppendFormattedText(RenderState* state, const char* text, MD_SIZE size)
{
	if (text == NULL || size == 0)
		return;

	FormattedSegment* seg = new FormattedSegment();
	seg->text.SetTo(text, size);
	seg->isBold   = state->isBold;
	seg->isItalic = state->isItalic;
	seg->isCode   = state->isCode;
	seg->isLink   = state->isLink;
	seg->url      = state->currentUrl;

	ContainerBlockType currentBlock = state->CurrentBlock();

	if (currentBlock == BLOCK_QUOTE && state->currentQuote != NULL) {
		state->currentQuote->segments.AddItem(seg);
	} else if (currentBlock == BLOCK_TABLE_CELL && state->currentCell != NULL) {
		state->currentCell->segments.AddItem(seg);
	} else {
		delete seg;
	}
}
