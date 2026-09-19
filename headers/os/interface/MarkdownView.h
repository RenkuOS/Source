#ifndef MARKDOWN_VIEW_H
#define MARKDOWN_VIEW_H

#include <TextView.h>
#include <String.h>
#include <Font.h>
#include <GraphicsDefs.h>

#include <md4c.h>

class BMarkdownView : public BTextView {
public:
							BMarkdownView(const char* name,
								uint32 flags = B_WILL_DRAW | B_NAVIGABLE);
							BMarkdownView(const char* name,
								const BFont* font, const rgb_color* color,
								uint32 flags = B_WILL_DRAW | B_NAVIGABLE);
	virtual					~BMarkdownView();

	// Supporto per BMessage/Archiving (se usato da LayoutBuilder / InterfaceKit)
	static	BArchivable*	Instantiate(BMessage* archive);

	// Imposta il testo Markdown ed esegue il parsing
	status_t				SetMarkdown(const char* markdownText);
	status_t				SetMarkdown(const BString& markdownText);

	// Metodi virtuali di BView per il BeAPI Layout System
	// virtual BSize			MinSize() override;
	// virtual BSize			PreferredSize() override;
	// virtual BSize			MaxSize() override;

private:
	// Struttura di stato interna usata dal parser durante il traversal di MD4C
	struct RenderState {
		BMarkdownView*		view;
		BFont				baseFont;
		BFont				currentFont;
		rgb_color			textColor;
		rgb_color			codeColor;
		
		bool				isBold;
		bool				isItalic;
		bool				isCode;
		bool				isBlockCode;
		uint32				headingLevel;
		
		RenderState()
			: view(NULL),
			  textColor(make_color(0, 0, 0)),
			  codeColor(make_color(220, 50, 50)),
			  isBold(false),
			  isItalic(false),
			  isCode(false),
			  isBlockCode(false),
			  headingLevel(0)
		{}
	};

	void					_Init();
	void					_ApplyCurrentStyle(int32 startPos, RenderState& state);

	// Callbacks C richieste da MD4C
	static int				_EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata);
	static int				_LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata);
	static int				_EnterSpanCb(MD_SPANTYPE type, void* detail, void* userdata);
	static int				_LeaveSpanCb(MD_SPANTYPE type, void* detail, void* userdata);
	static int				_TextCb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata);

	BString					fRawMarkdown;
};

#endif // MARKDOWN_VIEW_H
