/*
 * NextZipPlugin.cpp — Nextpad++ Linux plugin shell for NextZip.
 *
 * The shell over the shared NextZipController. Owns the NppData, implements
 * the NextZipHost bridge (open-in-editor via NPPM_DOOPEN, current-file via
 * NPPM_GETFULLCURRENTPATH, the dock panel register/show/hide), and wires
 * NPPN_FILESAVED → save-back-into-archive. The macOS SDK shape comes from the
 * shared NppPluginInterfaceLinux.h + LinuxViewBridge.cpp adapter.
 *
 * NextZip 2026 (GPL). Engine: 7-Zip (LGPL + unRAR restriction).
 */
#include "NppPluginInterfaceLinux.h"
#include "Scintilla.h"
#include <gtk/gtk.h>
#include "NextZipController.h"
#include "NextZipDialogs.h"
#include "NextZipHost.h"

static const char* PLUGIN_NAME = "NextZip";
static const int   NB_FUNC = 5;
static FuncItem    funcItem[NB_FUNC];
static NppData     nppData;

// ── host bridge: routes the controller's host calls to Nextpad++ ─────────────
struct NextZipPluginHost : NextZipHost {
	NextZipController* controller = nullptr;
	intptr_t           panelHandle = 0;   // NPPM_DMM_REGISTERPANEL handle
	bool               panelVisible = false;

	void openExtractedFile(const std::string&, const char* stablePath) override {
		// The Linux host's DOOPEN opens synchronously, but the stable map
		// pointer is passed anyway (macOS contract; costs nothing).
		nppData._sendMessage(nppData._nppHandle, NPPM_DOOPEN, 0, (intptr_t)stablePath);
	}

	std::string currentFilePath() override {
		char path[4096]; path[0] = 0;
		nppData._sendMessage(nppData._nppHandle, NPPM_GETFULLCURRENTPATH, sizeof(path), (intptr_t)path);
		return path;
	}

	void registerIfNeeded() {
		if (panelHandle) return;
		GtkWidget* v = controller ? controller->panelView() : nullptr;
		if (!v) return;
		// Linux ABI: wParam = panel title, lParam = the GtkWidget* (REVERSED
		// vs macOS — PORTING_NOTES trap #8; the host rejects a swapped call).
		panelHandle = nppData._sendMessage(nppData._nppHandle, NPPM_DMM_REGISTERPANEL,
		                                   (uintptr_t)"NextZip", (intptr_t)v);
		// Declare the reopen command so the host restores the panel after a
		// restart (GH linux#18): module = getName() ("NextZip"), cmdIndex 0 =
		// "Show NextZip Archive Manager". Hosts < 1.1.0 return 0 — ignored.
		if (panelHandle) {
			NppPanelInfo info;
			info.moduleName = PLUGIN_NAME;
			info.cmdIndex   = 0;
			nppData._sendMessage(nppData._nppHandle, NPPM_DMM_SETPANELINFO,
			                     (uintptr_t)panelHandle, (intptr_t)&info);
		}
	}

	void revealPanel() override {
		registerIfNeeded();
		if (panelHandle)
			nppData._sendMessage(nppData._nppHandle, NPPM_DMM_SHOWPANEL, (uintptr_t)panelHandle, 0);
		panelVisible = true;
	}

	void togglePanel() {
		registerIfNeeded();
		panelVisible = !panelVisible;
		if (panelHandle)
			nppData._sendMessage(nppData._nppHandle,
				panelVisible ? NPPM_DMM_SHOWPANEL : NPPM_DMM_HIDEPANEL,
				(uintptr_t)panelHandle, 0);
	}
};

static NextZipController* g_controller = nullptr;
static NextZipPluginHost* g_host       = nullptr;

static NextZipController* controller() {
	if (!g_controller) {
		g_controller = new NextZipController();
		g_host = new NextZipPluginHost();
		g_host->controller = g_controller;
		g_controller->setHost(g_host);
		GtkWidget* w = (GtkWidget*)cpHostWindow();
		NextZipDialogs::setParentWindow((w && GTK_IS_WINDOW(w)) ? GTK_WINDOW(w) : nullptr);
	}
	return g_controller;
}

// ── menu commands ─────────────────────────────────────────────────────────────
static void cmdShowPanel()   { (void)controller(); g_host->togglePanel(); }
static void cmdOpenArchive() { controller()->showOpenPanel(); }
static void cmdOpenCurrent() { controller()->openCurrentEditorFile(); }
static void cmdAbout()       { controller()->showAbout(); }

// ── exports ───────────────────────────────────────────────────────────────────
static void setItem(int idx, const char* name, PFUNCPLUGINCMD fn) {
	strlcpy(funcItem[idx]._itemName, name, NPP_MENU_ITEM_SIZE);
	funcItem[idx]._pFunc = fn;
	funcItem[idx]._init2Check = false;
}

extern "C" NPP_EXPORT void setInfo(LinuxHostNppData data) {
	cpBridgeInit(&data);
	nppData._nppHandle             = kHandleNpp;
	nppData._scintillaMainHandle   = kHandleScintillaMain;
	nppData._scintillaSecondHandle = kHandleScintillaSub;
	nppData._sendMessage           = cpSendMessage;

	memset(funcItem, 0, sizeof(funcItem));
	setItem(0, "Show NextZip Archive Manager", cmdShowPanel);
	setItem(1, "Open Archive…",                cmdOpenArchive);
	setItem(2, "Open Current File as Archive", cmdOpenCurrent);
	setItem(3, "-",                            nullptr);   // separator ("-" on this host; macOS used "—")
	setItem(4, "About NextZip",                cmdAbout);
}

extern "C" NPP_EXPORT const char* getName() { return PLUGIN_NAME; }

extern "C" NPP_EXPORT FuncItem* getFuncsArray(int* nbF) { *nbF = NB_FUNC; return funcItem; }

extern "C" NPP_EXPORT void beNotified(SCNotification* n) {
	if (!n) return;
	if (n->nmhdr.code == NPPN_FILESAVED && g_controller) {
		// Map the saved buffer to its path; if it's a file we extracted from an
		// archive, write the edits back into the archive.
		char path[4096]; path[0] = 0;
		nppData._sendMessage(nppData._nppHandle, NPPM_GETFULLPATHFROMBUFFERID,
		                     (uintptr_t)n->nmhdr.idFrom, (intptr_t)path);
		if (path[0]) g_controller->handleFileSaved(path);
	}
}

extern "C" NPP_EXPORT intptr_t messageProc(uint32_t, uintptr_t, intptr_t) { return 1; }
extern "C" NPP_EXPORT int isUnicode() { return 1; }
