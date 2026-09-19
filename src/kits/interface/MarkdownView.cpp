#include "MarkdownView.h"

#include <InterfaceDefs.h>
#include <algorithm>

BMarkdownView::BMarkdownView(const char* name, uint32 flags)
    :
    BTextView(name, flags)
{
    MakeEditable(false);
    MakeSelectable(true);
    SetStylable(true);
}

BMarkdownView::~BMarkdownView()
{
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

    if (markdownText == NULL || strlen(markdownText) == 0)
        return B_OK;

    RenderState state;
    state.view = this;
    
    // Recuperiamo il font base impostato sulla vista
    //GetFontAndColor(0, &state.baseFont); no diventa incrementale
    SetFontAndColor(be_plain_font);
    GetFont(&state.baseFont);
    state.currentFont = state.baseFont;
    
    // Colori di default basati sul tema di sistema
    state.textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
    state.codeColor = (rgb_color){ 200, 40, 40, 255 }; // Rosso scuro per il codice

    MD_PARSER parser = {
        0,                      // abi_version
        MD_FLAG_TABLES,         // flags
        _EnterBlockCb,
        _LeaveBlockCb,
        _EnterSpanCb,
        _LeaveSpanCb,
        _TextCb,
        NULL,                   // debug_log
        NULL                    // syntax_fallback
    };

    int result = md_parse(markdownText, (MD_SIZE)strlen(markdownText), &parser, &state);
    return (result == 0) ? B_OK : B_ERROR;
}
void
BMarkdownView::_ApplyCurrentStyle(int32 startPos, RenderState& state)
{
    int32 endPos = TextLength();
    if (startPos >= endPos)
        return;

    // Calcolo stile del font
    uint16 face = B_REGULAR_FACE;
    if (state.isBold || state.headingLevel > 0)
        face |= B_BOLD_FACE;
    if (state.isItalic)
        face |= B_ITALIC_FACE;

    state.currentFont.SetFace(face);

    // Calcolo dimensione del font se siamo in un Heading
    if (state.headingLevel > 0) {
        float factor = 1.0f + (0.15f * (7 - std::min(state.headingLevel, (uint32)6)));
        state.currentFont.SetSize(state.baseFont.Size() * factor);
    } else {
        state.currentFont.SetSize(state.baseFont.Size());
    }

    // Selezione colore
    rgb_color colorToApply = (state.isCode || state.isBlockCode) 
        ? state.codeColor 
        : state.textColor;

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
            break;
        case MD_BLOCK_UL:
        case MD_BLOCK_OL:
        case MD_BLOCK_LI:
            // Gestione opzionale dei rientri o pallini
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
        case MD_BLOCK_CODE:
            state->isBlockCode = false;
            state->view->Insert("\n\n");
            break;
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
    
    // Estraiamo il testo dal buffer MD4C
    BString str(text, size);
    state->view->Insert(str.String());

    // Passiamo *state a _ApplyCurrentStyle
    state->view->_ApplyCurrentStyle(startPos, *state);

    return 0;
}
