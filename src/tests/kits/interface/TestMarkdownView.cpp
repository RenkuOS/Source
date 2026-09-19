#include <Application.h>
#include <Window.h>
#include <LayoutBuilder.h>
#include <ScrollView.h>
#include <Button.h>
#include <StringView.h>
#include <MarkdownView.h>

// ID dei comandi per la BWindow
enum {
	MSG_SET_SAMPLE_MARKDOWN = 'mksp',
	MSG_CLEAR_MARKDOWN      = 'mkcl'
};

// --- Finestra di Test ---
class TestMarkdownWindow : public BWindow {
public:
	TestMarkdownWindow()
		: BWindow(BRect(100, 100, 700, 550), "MarkdownView Test", 
		          B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE)
	{
		// 1. Istanziamo BMarkdownView
		fMarkdownView = new BMarkdownView("markdown_view");
		fMarkdownView->MakeEditable(false); // Di solito il viewer non è editabile dall'utente

		// 2. Mettiamo la vista dentro BScrollView
		fScrollView = new BScrollView("markdown_scroll", fMarkdownView, 0, false, true);

		// 3. Pulsanti di controllo
		BButton* sampleBtn = new BButton("sample_btn", "Carica Sample", 
		                                 new BMessage(MSG_SET_SAMPLE_MARKDOWN));
		BButton* clearBtn  = new BButton("clear_btn", "Pulisci", 
		                                 new BMessage(MSG_CLEAR_MARKDOWN));

		// 4. Costruzione del Layout responsive usando BLayoutBuilder
		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_DEFAULT_SPACING)
			.Add(new BStringView("title", "Test rendering nativo Markdown (MD4C):"))
			.Add(fScrollView, 1.0) // La vista Markdown prende tutto lo spazio verticale
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.AddGlue()
				.Add(clearBtn)
				.Add(sampleBtn)
			.End();

		// Carichiamo il contenuto iniziale
		_LoadDefaultContent();
	}

	virtual void MessageReceived(BMessage* message) override
	{
		switch (message->what) {
			case MSG_SET_SAMPLE_MARKDOWN:
				_LoadDefaultContent();
				break;

			case MSG_CLEAR_MARKDOWN:
				fMarkdownView->SetMarkdown("");
				break;

			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

private:
	void _LoadDefaultContent()
	{
		fMarkdownView->SetMarkdown(
			"# Ahoy Pirate! 🏴‍☠️\n\n"
			"Benvenuto in **Haiku OS Pirate Edition**.\n\n"
			"### Funzionalità principali:\n"
			"* Supporto *Markdown* nativo mediante `MD4C`.\n"
			"* Integrazione completa col **BeAPI** Layout System.\n"
			"* Performance elevate e basso consumo di memoria.\n\n"
			"---\n\n"
			"### Esempio di codice inline e blocchi:\n"
			"Usa il comando `pkgman install` nel terminale per installare i pacchetti.\n\n"
			"```cpp\n"
			"// Esempio C++\n"
			"BMarkdownView* view = new BMarkdownView(\"md\");\n"
			"```\n\n"
			"> \"L'eleganza di BeOS incontra la potenza del parsing moderno.\"\n"
		);
	}

	BMarkdownView* fMarkdownView;
	BScrollView*   fScrollView;
};

// --- Applicazione ---
class TestMarkdownApp : public BApplication {
public:
	TestMarkdownApp()
		: BApplication("application/x-vnd.Haiku-TestMarkdownView")
	{
	}

	virtual void ReadyToRun() override
	{
		TestMarkdownWindow* window = new TestMarkdownWindow();
		window->Show();
	}
};

int main()
{
	TestMarkdownApp app;
	app.Run();
	return 0;
}
