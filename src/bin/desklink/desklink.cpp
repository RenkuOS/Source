/*
 * Copyright 2003-2009, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jérôme Duval
 *		François Revol
 *		Marcus Overhagen
 *		Jonas Sundström
 */

//! VolumeControl and link items in Deskbar

#include "desklink.h"

#include <stdio.h>
#include <stdlib.h>

#include <Alert.h>
#include <Application.h>
#include <ControlLook.h>
#include <Deskbar.h>
#include <MimeType.h>
#include <Roster.h>
#include <String.h>

#include "DeskButton.h"
#include "VolumeWindow.h"


const char *kAppSignature = "application/x-vnd.Haiku-desklink";
	// the application signature used by the replicant to find the
	// supporting code


status_t
our_image(image_info& image)
{
	int32 cookie = 0;
	while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
		if ((char*)our_image >= (char*)image.text
			&& (char*)our_image <= (char*)image.text + image.text_size) {
			return B_OK;
		}
	}

	return B_ERROR;
}

int
main(int argc, char **argv)
{
	BApplication app(kAppSignature);
	bool atLeastOnePath = false;
	BList titleList;
	BList actionList;
	BDeskbar deskbar;
	status_t err = B_OK;
	
	BMessage persistMsg;
	BPath settingsDir;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsDir) == B_OK) {
		BPath ROdsklnkPth(settingsDir.Path(), "desklink");
		BFile ROdsklnkFile(ROdsklnkPth.Path(), B_READ_ONLY);
		if (ROdsklnkFile.InitCheck() == B_OK) {
			//il file esiste! aggiorniamolo così poi aggiungeremo invece di sovrascrivere
			if (persistMsg.Unflatten(&ROdsklnkFile) != B_OK)
				printf("Error reading previously saved desklinks\ngonna overwrite it!");
		}
	}

	BMessage* appToSave = new BMessage();
	// TODO: caricare il vecchio persistMsg flattenizzato su disco
	BString completeCmd;
	for (int32 i = 1; argv[i] != NULL; i++) {
		EscapeArgument(argv[i], completeCmd);

		if (argv[i + 1] != NULL)
			completeCmd << " ";
	}

	appToSave->AddString("completeCmd", completeCmd);
	
	bool correctCmd = true;
	
	bool removePresence = false;
	for (int32 i = 1; argv[i]!=NULL; i++) {
		if (strcmp(argv[i], "--help") == 0)
			break;

		if (strcmp(argv[i], "--list") == 0) {
			int32 count = deskbar.CountItems();
			int32 found = 0;
			int32 j = 0;
			printf("Deskbar items:\n");

			while (found < count) {
				const char *name = NULL;
				if (deskbar.GetItemInfo(j, &name) == B_OK) {
					printf("Item %" B_PRId32 ": '%s'\n", j, name);
					free((void *)name);
					found++;
				}
				j++;
			}
			return 0;
		}

		if (strcmp(argv[i], "--add-volume") == 0) {
			entry_ref ref;
			if (get_ref_for_path(argv[0], &ref) == B_OK) {
				deskbar.AddItem(&ref);
			}
			return 0;
		}

		if (strcmp(argv[i], "--volume-control") == 0) {
			BWindow* window = new VolumeWindow(BRect(200, 150, 400, 200));
			window->Show();

			wait_for_thread(window->Thread(), NULL);
			return 0;
		}

		if (strncmp(argv[i], "--remove", 8) == 0) {
			BString replicant = "DeskButton";
			if (strncmp(argv[i] + 8, "=", 1) == 0) {
				if (strlen(argv[i] + 9) > 0) {
					replicant = argv[i] + 9;
				} else {
					printf("desklink: Missing replicant name.\n");
					return 1;
				}
			}
			int32 found = 0;
			int32 found_id;
			while (deskbar.GetItemInfo(replicant.String(), &found_id) == B_OK) {
				err = deskbar.RemoveItem(found_id);
				if (err != B_OK) {
					printf("desklink: Error removing replicant id "
						"%" B_PRId32 ": %s\n", found_id, strerror(err));
					break;
				}
				found++;
			}
			printf("Removed %" B_PRId32 " items.\n", found);
			// --- AGGIORNAMENTO FILE DI PERSISTENZA ---
			if (found > 0) {
				BPath settingsDir;
				if (find_directory(B_USER_SETTINGS_DIRECTORY, &settingsDir) == B_OK) {
					BPath desklinkPath(settingsDir.Path(), "desklink");
					BFile desklinkFile(desklinkPath.Path(), B_READ_WRITE);

					if (desklinkFile.InitCheck() == B_OK) {
						BMessage persistMsg;
						if (persistMsg.Unflatten(&desklinkFile) == B_OK) {
							type_code type;
							int32 appCount = 0;
							persistMsg.GetInfo("app", &type, &appCount);

							bool modified = false;

							// Scorriamo tutti i comandi salvati (al contrario per rimozione sicura)
							for (int32 k = appCount - 1; k >= 0; k--) {
								BMessage appMsg;
								if (persistMsg.FindMessage("app", k, &appMsg) == B_OK) {
									const char* cmdStr = NULL;
									if (appMsg.FindString("refname", &cmdStr) == B_OK) {
										// Se il comando salvato conteneva il nome del replicant rimosso
										if (strcmp(cmdStr, replicant.String()) == 0) {
											persistMsg.RemoveData("app", k);
											modified = true;
										}
									}
								}
							}

							if (modified) {
								// Svuotiamo il file e riscriviamo il BMessage aggiornato
								desklinkFile.SetSize(0);
								desklinkFile.Seek(0, SEEK_SET);
								persistMsg.Flatten(&desklinkFile);
							}
						}
					}
				}
			}
			return err;
		}

		if (strncmp(argv[i], "cmd=", 4) == 0) {
			BString *title = new BString(argv[i] + 4);
			int32 index = title->FindFirst(':');
			if (index <= 0) {
				printf("desklink: usage: cmd=title:action\n");
				correctCmd=false;
				printf("          this desklink won't be persistent\n");
			} else {
				title->Truncate(index);
				BString *action = new BString(argv[i] + 4);
				action->Remove(0, index+1);
				const char* lookfor = "desklink --remove";
				if (action->FindFirst(lookfor)>-1) removePresence=true;
				BMessage* param = new BMessage();
				param->AddString("title",title->String());
				param->AddString("action",action->String());
				appToSave->AddMessage("parameter",param);
				titleList.AddItem(title);
				actionList.AddItem(action);
			}
			continue;
		}

		atLeastOnePath = true;

		BEntry entry(argv[i], true);
		entry_ref ref;

		if (entry.Exists()) {
			entry.GetRef(&ref);
		} else if (BMimeType::IsValid(argv[i])) {
			if (be_roster->FindApp(argv[i], &ref) != B_OK) {
				printf("desklink: cannot find '%s'\n", argv[i]);
				return 1;
			}
		} else {
			printf("desklink: cannot find '%s'\n", argv[i]);
			return 1;
		}
		
		if (!removePresence) {
			//aggiungiamo sempre una rimozione
			BString *remtitle = new BString("Remove replicant");
			BString *remaction = new BString("desklink --remove=");
			remaction->Append(ref.name);
			titleList.AddItem(remtitle);
			actionList.AddItem(remaction);
		}
		appToSave->AddString("refname", ref.name);

		err = deskbar.AddItem(&ref);
		if (err != B_OK) {
			const float height = deskbar.MaxItemHeight();
			err = deskbar.AddItem(new DeskButton(BRect(BPoint(0, 0),
					BSize(height, height)),
				&ref, ref.name, titleList, actionList));
			if (err != B_OK) {
				printf("desklink: Deskbar refuses link to '%s': %s\n", argv[i], strerror(err));
				return 1;
			}
			// se correctCmd aggiungere la nuova app a persistMSG e flattenizzarla su disco
			if (correctCmd) {
				//prima verifichiamo che la nostra app non sia già presente, verificando il comando
				BMessage fApp;
				bool tosave = true;
				type_code type;
				int32 count = 0;
				persistMsg.GetInfo("app", &type, &count);
				for (int32 v=0; v<count; v++) {
					if (persistMsg.FindMessage("app",v, &fApp)==B_OK) {
						const char* savedCmd;
						if (fApp.FindString("completeCmd", &savedCmd)==B_OK){
							if (strcmp(savedCmd, completeCmd.String()) == 0){
								tosave = false;
								printf("This app is already persistent\n");
							}
						}
					}
				}
				if (tosave) {
					persistMsg.AddMessage("app",appToSave);
					BPath desklinkPath(settingsDir.Path(), "desklink");
					BFile desklinkFile(desklinkPath.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
					if (desklinkFile.InitCheck() != B_OK) {
						return desklinkFile.InitCheck();
					}

					err = persistMsg.Flatten(&desklinkFile);
					if (err!=B_OK){
						printf("Unable to save desklink persistence\n");
					}
				}
			}
		}

		titleList.MakeEmpty();
		actionList.MakeEmpty();
	}

	if (!atLeastOnePath) {
		printf(	"usage: desklink { [ --list|--remove|[cmd=title:action ... ] [ path|signature ] } ...\n"
			"--add-volume: install volume control into Deskbar.\n"
			"--volume-control: show window with global volume control.\n"
			"--list: list all Deskbar addons.\n"
			"--remove: remove all desklink addons.\n"
			"--remove=name: remove all 'name' addons.\n");
		return 1;
	}

	return 0;
}
