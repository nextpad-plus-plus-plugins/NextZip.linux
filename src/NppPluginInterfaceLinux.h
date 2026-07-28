/*
 * NppPluginInterfaceLinux.h — macOS-SDK-shaped adapter for the Linux host.
 *
 * ComparePlus was ported from Windows to macOS against NppPluginInterfaceMac.h:
 *   - NppData carries three OPAQUE handles (npp / main sci / sub sci) and one
 *     _sendMessage(handle, msg, wParam, lParam) callback;
 *   - SCI_* messages sent to _scintillaMainHandle/_scintillaSecondHandle are
 *     resolved BY THE HOST to the currently-active tab of each split view;
 *   - several NPPM_* messages carry Windows dual-view semantics
 *     (GETPOSFROMBUFFERID packs the view id in bit 30, GETNBOPENFILES takes
 *     PRIMARY_VIEW/SECOND_VIEW, ACTIVATEDOC takes (view, index), ...).
 *
 * The Linux host's plugin ABI is flatter (hostMsg has no handle; plugins talk
 * to Scintilla widgets directly) and its plugin view model is single-view.
 * This header + LinuxViewBridge.cpp recreate the macOS shape ON THE PLUGIN
 * SIDE, so the ComparePlus sources stay near-identical to the macOS port:
 * sentinel handles route through cpSendMessage(), which resolves MAIN/SUB to
 * the live current tab of the host's primary / vertical-split notebooks and
 * implements the dual-view NPPM semantics over them. No host changes.
 *
 * Linux port by Andrew Letov, 2026 (GPL v3, as upstream).
 */
#pragma once

#include <cstdint>
#include <cstring>

// ── the Linux host contract, with its type names shielded ───────────────────
// plugin.h typedefs `NppData` and `FuncItem` — the same names this adapter must
// define with the macOS SHAPES. Rename the host's on the way in. (Not wrapped
// in extern "C": plugin.h includes <gtk/gtk.h>, which pulls in C++ headers.)
#define NppData  LinuxHostNppData
#define FuncItem LinuxHostFuncItem
#include "plugin.h"
#undef NppData
#undef FuncItem

// ── opaque handles (sentinels, resolved at every call — macOS model) ────────
typedef uintptr_t NppHandle;

static const NppHandle kHandleNpp           = 0x4E505000;  // "NPP\0"
static const NppHandle kHandleScintillaMain = 0x5343490A;  // "SCI\n"
static const NppHandle kHandleScintillaSub  = 0x5343490B;  // "SCI\v"

// ── view constants (Windows / macOS canon) ──────────────────────────────────
#define MAIN_VIEW          0
#define SUB_VIEW           1
#define ALL_OPEN_FILES     0
#define PRIMARY_VIEW       1
#define SECOND_VIEW        2

#define STATUSBAR_DOC_TYPE 0

#define NPP_MENU_ITEM_SIZE 64

// ── macOS-shaped plugin structs ─────────────────────────────────────────────
typedef intptr_t (*NppSendMessageFn)(NppHandle handle, unsigned int msg,
                                     uintptr_t wParam, intptr_t lParam);

struct NppData
{
    NppHandle        _nppHandle;
    NppHandle        _scintillaMainHandle;
    NppHandle        _scintillaSecondHandle;
    NppSendMessageFn _sendMessage;
};

struct ShortcutKey
{
    bool          _isCtrl;
    bool          _isAlt;
    bool          _isShift;
    bool          _isCmd;      // macOS extension; unused on Linux
    unsigned char _key;
};

typedef void (*PFUNCPLUGINCMD)();

// LAYOUT-CRITICAL: the host indexes the array returned by getFuncsArray() with
// ITS OWN FuncItem stride, so this MUST match the host's struct exactly
// (char[64], fn, int, int). The Linux host FuncItem has no _pShKey field, so
// there is none here — the plugin's ShortcutKey assignments are dropped in the
// .cpp (Linux has no per-item plugin shortcuts).
struct FuncItem
{
    char            _itemName[NPP_MENU_ITEM_SIZE];
    PFUNCPLUGINCMD  _pFunc;
    int             _cmdID;
    int             _init2Check;
};

#ifndef NPP_EXPORT
#define NPP_EXPORT __attribute__((visibility("default")))
#endif

// strlcpy is a BSDism; glibc (until 2.38) spells it differently.
#ifndef strlcpy
#define strlcpy(dst, src, n) (g_strlcpy((dst), (src), (n)))
#endif

// ── the adapter entry points (LinuxViewBridge.cpp) ──────────────────────────

// The routing _sendMessage put into NppData. NPPM messages with dual-view
// semantics are implemented plugin-side (see the bridge); everything else is
// forwarded to the host verbatim. SCI messages to the sentinel handles resolve
// to the current tab of the matching notebook (or a set redirect).
intptr_t cpSendMessage(NppHandle handle, unsigned int msg, uintptr_t wParam, intptr_t lParam);

// Initialise the bridge from the host's real NppData (called from setInfo).
void cpBridgeInit(const LinuxHostNppData* host);

// Route CallScintilla(view) straight to a specific ScintillaView widget
// (macOS setScintillaRedirect equivalent). NULL restores default routing.
void setScintillaRedirect(int viewNum, void* sciWidget);

// Host UI actions (macOS hostAction(@selector(...)) equivalents), driven
// through the host's PUBLIC GAction names — no host internals invoked.
void cpHostMoveToOtherVerticalView();   // app.move-to-vview (a toggle, like macOS)
void cpHostResetView();                 // app.reset-view

// The host's REAL main window widget (the sentinel _nppHandle is NOT a widget).
void*    cpHostWindow();

// Live per-view resolution helpers used by the plugin layer.
void*    cpViewSci(int viewNum);        // current tab's ScintillaView of a view (may be NULL for SUB)
int      cpViewCount(int viewNum);      // open tabs in a view (0 if the split doesn't exist)
intptr_t cpCurrentViewId();             // which view holds the focused document
void*    cpBufferSci(intptr_t buffId);  // the (stable) ScintillaView owning a buffer, or NULL
int      cpTotalFileCount();            // primary + vertical-split tab count
