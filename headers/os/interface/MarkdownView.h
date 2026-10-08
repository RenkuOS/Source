/*
 * Copyright 2026, Pirati Del Frico
 * All rights reserved. Distributed under the terms of the MIT license.
 * SPDX-License-Identifier: MIT
 */
#ifndef MARKDOWN_VIEW_H
#define MARKDOWN_VIEW_H

#include <TextView.h>
#include <String.h>
#include <Font.h>
#include <Bitmap.h>
#include <GraphicsDefs.h>
#include <ObjectList.h>
#include <Cursor.h>
#include <md4c.h>
#include <stack>

struct FormattedSegment {
	BString  text;
	bool     isBold;
	bool     isItalic;
	bool     isCode;
	bool     isLink;
	BString  url;

	FormattedSegment()
		: isBold(false), isItalic(false),
		  isCode(false), isLink(false) {}
};

struct QuoteRegion {
	int32       startPos;
	int32       endPos;
	BRect       copyRect;
	BObjectList<FormattedSegment, true> segments;

	QuoteRegion() : startPos(-1), endPos(-1), segments(10) {}
	BString ToText() const {
		BString result;
		int32 count = segments.CountItems();
		for (int32 i = 0; i < count; i++) {
			FormattedSegment* seg = segments.ItemAt(i);
			if (seg != NULL)
				result.Append(seg->text);
		}
		return result;
	}
};

struct HorizontalRuleRegion {
	int32 pos;
};

struct LinkRegion {
	int32   startPos;
	int32   endPos;
	BString url;

	LinkRegion() : startPos(-1), endPos(-1) {}
};

struct TableCellRegion {
	int32       startPos;
	int32       endPos;
	int32       colIndex;
	BObjectList<FormattedSegment, true> segments;

	TableCellRegion() : startPos(-1), endPos(-1), colIndex(0), segments(5) {}
	BString ToText() const {
		BString result;
		int32 count = segments.CountItems();
		for (int32 i = 0; i < count; i++) {
			FormattedSegment* seg = segments.ItemAt(i);
			if (seg != NULL)
				result.Append(seg->text);
		}
		return result;
	}
};

// Tipi di blocchi per lo Stack
enum ContainerBlockType {
	BLOCK_NONE = 0,
	BLOCK_QUOTE,
	BLOCK_TABLE_CELL,
	BLOCK_CODE
};

struct TableRowRegion {
	int32 startPos;
	int32 endPos;
	bool  isHeader;
	BObjectList<TableCellRegion, true> cells;

	TableRowRegion()
		: startPos(-1),
		  endPos(-1),
		  isHeader(false),
		  cells(10)
	{}
};

struct TableRegion {
	int32 startPos;
	int32 endPos;
	BObjectList<TableRowRegion, true> rows;
	BRect copyRect;
	TableRegion() : rows(10) {}
	BString ToText() const {
		BString result;
		int32 rowCount = rows.CountItems();
		for (int32 r = 0; r < rowCount; r++) {
			TableRowRegion* row = rows.ItemAt(r);
			if (row == NULL) continue;

			int32 cellCount = row->cells.CountItems();
			for (int32 c = 0; c < cellCount; c++) {
				TableCellRegion* cell = row->cells.ItemAt(c);
				if (cell != NULL)
					result.Append(cell->ToText());

				if (c < cellCount - 1)
					result.Append("\t");
			}
			result.Append("\n");
		}
		return result;
	}
};

struct ImageRegion {
	int32    startPos;
	int32    endPos;
	BString  src;
	BString  alt;
	BBitmap* bitmap;

	ImageRegion()
		: startPos(-1), endPos(-1), bitmap(NULL) {}

	~ImageRegion() {
		delete bitmap;
	}
};

struct CodeBlockRegion {
	int32   startPos;
	int32   endPos;
	BString codeText;
	BRect   copyRect;
};

class BMarkdownView : public BTextView {
public:
							BMarkdownView(const char* name,
								uint32 flags = B_WILL_DRAW | B_NAVIGABLE);
							BMarkdownView(const char* name,
								const BFont* font, const rgb_color* color,
								uint32 flags = B_WILL_DRAW | B_NAVIGABLE);
							BMarkdownView(BMessage* archive);
	virtual					~BMarkdownView();

	static	BArchivable*	Instantiate(BMessage* archive);
	virtual	status_t		Archive(BMessage* archive, bool deep = true) const;
	
	virtual void			Draw(BRect updateRect);
	virtual void			FrameResized(float width, float height);
	
	virtual void			MouseDown(BPoint where);
	virtual void			MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage);

	void					InsertRaw(const char* text);
	void					InsertRaw(const char* text, int32 length);
	void					InsertRaw(int32 offset, const char* text, int32 length);
	int32					RawTextLength() const;
	const char*				RawText() const;

	status_t				SetMarkdown(const char* markdownText);
	status_t				SetMarkdown(const BString& markdownText);
	void					CopyRawMarkdownToClipboard();
	void					CopyPlainTextToClipboard();

private:
	BMarkdownView(const BMarkdownView&);
	BMarkdownView& operator=(const BMarkdownView&);

	// 1. Spostato RenderState IN CIMA alla sezione private
	struct RenderState {
		BMarkdownView*   view;
		BFont            currentFont;
		rgb_color        textColor;
		rgb_color        codeColor;
		
		bool             isBold;
		bool             isItalic;
		bool             isCode;
		bool             isBlockCode;
		CodeBlockRegion* currentCodeBlock;
		uint32           headingLevel;
		int32            currentBlockStart;
		bool             isTable;
		bool             isHeaderRow;
		TableRegion*     currentTable;
		int32            currentColIndex;
		TableCellRegion* currentCell;
		bool             isImage;
		ImageRegion*     currentImage;
		BString          currentImageAlt;
		bool             isLink;
		BString          currentUrl;
		LinkRegion*      currentLink;
		int32            listDepth;
		bool             isOrderedList;
		int32            olItemNumber;
		bool             isQuote;
		QuoteRegion*     currentQuote;
		
		std::stack<ContainerBlockType> blockStack;

		ContainerBlockType CurrentBlock() const {
			if (blockStack.empty())
				return BLOCK_NONE;
			return blockStack.top();
		}

		RenderState()
			: view(NULL),
			  textColor(make_color(0, 0, 0)),
			  codeColor(make_color(220, 50, 50)),
			  isBold(false),
			  isItalic(false),
			  isCode(false),
			  isBlockCode(false),
			  currentCodeBlock(NULL),
			  headingLevel(0),
			  currentBlockStart(-1),
			  isTable(false),
			  isHeaderRow(false),
			  currentTable(NULL),
			  currentColIndex(0),
			  currentCell(NULL),
			  isImage(false),
			  currentImage(NULL),
			  isLink(false),
			  currentUrl(""),
			  currentLink(NULL),
			  listDepth(0),
			  isOrderedList(false),
			  olItemNumber(1),
			  isQuote(false),
			  currentQuote(NULL)
		{}
	};

	BString                 _ConvertMarkdownToPlainText(const BString& markdown);
	void                    _Init();
	void                    _ClearRegions();
	void                    _ApplyCurrentStyle(int32 startPos, RenderState& state);
	void                    _AppendFormattedText(RenderState* state, const char* text, MD_SIZE size);
	void                    _DrawFormattedSegment(FormattedSegment* seg, BPoint& drawPt, float lineMaxAscent);

	static int              _EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata);
	static int              _LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata);
	static int              _EnterSpanCb(MD_SPANTYPE type, void* detail, void* userdata);
	static int              _LeaveSpanCb(MD_SPANTYPE type, void* detail, void* userdata);
	static int              _TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata);

	void                    _LoadImageForRegion(ImageRegion* region);
	LinkRegion*             _LinkAt(BPoint point) const;

	BString                 fRawMarkdown;
	BCursor                  fHandCursor;

	BObjectList<CodeBlockRegion, true> fCodeBlocks;
	BObjectList<TableRegion, true>     fTables;
	BObjectList<ImageRegion, true>     fImages;
	BObjectList<LinkRegion, true>      fLinks;
	BObjectList<QuoteRegion, true>     fQuotes;

	BList                   fHorizontalRules;
};

#endif // MARKDOWN_VIEW_H
