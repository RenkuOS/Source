#include <Application.h>
#include <Window.h>
#include <LayoutBuilder.h>
#include <MarkdownScrollView.h>
#include <Button.h>
#include <StringView.h>
#include <MarkdownView.h>
#include <Path.h>
#include <File.h>
#include <Entry.h>
#include <Message.h>
#include <TranslationUtils.h>

enum {
	MSG_SET_SAMPLE_MARKDOWN = 'mksp',
	MSG_CLEAR_MARKDOWN      = 'mkcl',
	MSG_LOAD_MARKDOWN_FILE  = 'mklf'
};

// --- Finestra di Test ---
class TestMarkdownWindow : public BWindow {
public:
	TestMarkdownWindow()
		: BWindow(BRect(100, 100, 800, 650), "MarkdownView Test - Full Features", 
				  B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	{
		// 1. Istanziamo BMarkdownView
		fMarkdownView = new BMarkdownView("markdown_view");
		fMarkdownView->MakeEditable(false);

		// 2. Mettiamo la vista dentro BMarkdownScrollView
		fScrollView = new BMarkdownScrollView("markdown_scroll", fMarkdownView,
						B_FOLLOW_ALL, 0, false, true);

		// 3. Pulsanti di controllo
		BButton* sampleBtn = new BButton("sample_btn", "Ricarica Sample", 
										 new BMessage(MSG_SET_SAMPLE_MARKDOWN));
		BButton* clearBtn  = new BButton("clear_btn", "Pulisci", 
										 new BMessage(MSG_CLEAR_MARKDOWN));

		// 4. Costruzione del Layout responsive usando BLayoutBuilder
		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_DEFAULT_SPACING)
			.Add(new BStringView("title", "Test rendering nativo Markdown (Links, Tabelle & Immagini):"))
			.Add(fScrollView, 1.0)
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.AddGlue()
				.Add(clearBtn)
				.Add(sampleBtn)
			.End();

		fLoadedFromFile = false;
	}

	virtual void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_SET_SAMPLE_MARKDOWN:
				LoadDefaultContent();
				break;

			case MSG_CLEAR_MARKDOWN:
				fMarkdownView->SetMarkdown("");
				break;

			case MSG_LOAD_MARKDOWN_FILE: {
				entry_ref ref;
				if (message->FindRef("refs", &ref) == B_OK) {
					LoadFileContent(&ref);
				}
				break;
			}

			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

	void LoadDefaultContent()
	{
		fLoadedFromFile = false;
		fMarkdownView->SetMarkdown(
			"# Ahoy Pirate! 🏴‍☠️\n\n"
			"Benvenuto nel test avanzato di **BMarkdownView** per Haiku OS.\n\n"
			"### 🔗 Test Collegamenti Ipertestuali (BUrl / B_LINK_TEXT_COLOR):\n"
			"* Visita il sito ufficiale: [Haiku OS Website](https://www.haiku-os.org)\n"
			"* Apri il tracker dei bug: [Haiku Bugtracker](https://dev.haiku-os.org)\n"
			"* Invia un'email di test: [Dev Mail](mailto:developer@example.com)\n\n"
			"---\n\n"
			"### 📊 Test Tabelle (Zebra Striping & Fixed Font):\n\n"
			"| Componente | Tipo | Stato |\n"
			"| :--- | :---: | ---: |\n"
			"| `MD4C Parser` | C Library | ✅ Integrato |\n"
			"| `BTextView` | Native View | ✅ Esteso |\n"
			"| `BUrl` | Support Kit | ✅ Attivo |\n"
			"| `Translation Kit` | Graphics | ✅ Operativo |\n\n"
			"---\n\n"
			"### 🖼️ Test Immagini (Translation Kit & BBitmap):\n"
			"Di seguito un'icona di sistema di Haiku caricata via percorso locale:\n\n"
			"![Haiku Logo](/boot/home/Progjets/haiku/data/artwork/logo.png)\n\n"
			"---\n\n"
			"### 💻 Codice e Quotes:\n"
			"```cpp\n"
			"// Test apertura URL con il colore nativo di sistema\n"
			"BUrl url(\"[https://www.haiku-os.org](https://www.haiku-os.org)\");\n"
			"if (url.IsValid())\n"
			"    url.OpenWithPreferredApp();\n"
			"```\n\n"
			"> \"L'eleganza di BeOS incontra la potenza del parsing moderno.\"\n"
		);
	}

	void LoadFileContent(const entry_ref* ref)
	{
		BFile file(ref, B_READ_ONLY);
		if (file.InitCheck() != B_OK)
			return;

		off_t size = 0;
		file.GetSize(&size);
		if (size <= 0)
			return;

		BString buffer;
		char* charBuffer = buffer.LockBuffer(size + 1);
		if (charBuffer == NULL)
			return;

		file.Read(charBuffer, size);
		charBuffer[size] = '\0';
		buffer.UnlockBuffer();

		fMarkdownView->SetMarkdown(buffer);

		BPath path(ref);
		if (path.InitCheck() == B_OK) {
			BString title("MarkdownView Test - ");
			title.Append(path.Leaf());
			SetTitle(title.String());
		}

		fLoadedFromFile = true;
	}

	bool HasLoadedFromFile() const { return fLoadedFromFile; }

private:
	BMarkdownView*       fMarkdownView;
	BMarkdownScrollView* fScrollView;
	bool                 fLoadedFromFile;
};

// --- Applicazione ---
class TestMarkdownApp : public BApplication {
public:
	TestMarkdownApp()
		: BApplication("application/x-vnd.Haiku-TestMarkdownView"),
		  fWindow(NULL),
		  fHasCustomFile(false)
	{
	}

	virtual void ArgvReceived(int32 argc, char** argv) override
	{
		if (argc > 1) {
			entry_ref ref;
			if (get_ref_for_path(argv[1], &ref) == B_OK) {
				fFileRef = ref;
				fHasCustomFile = true;
			}
		}
	}

	virtual void RefsReceived(BMessage* message) override
	{
		entry_ref ref;
		if (message->FindRef("refs", &ref) == B_OK) {
			fFileRef = ref;
			fHasCustomFile = true;

			if (fWindow != NULL) {
				BMessage loadMsg(MSG_LOAD_MARKDOWN_FILE);
				loadMsg.AddRef("refs", &fFileRef);
				fWindow->PostMessage(&loadMsg);
			}
		}
	}

	virtual void ReadyToRun() override
	{
		fWindow = new TestMarkdownWindow();

		if (fHasCustomFile) {
			fWindow->LoadFileContent(&fFileRef);
		} else {
			fWindow->LoadDefaultContent();
		}

		fWindow->Show();
	}

private:
	TestMarkdownWindow* fWindow;
	entry_ref           fFileRef;
	bool                fHasCustomFile;
};

int main(int argc, char** argv)
{
	TestMarkdownApp app;
	app.Run();
	return 0;
}
