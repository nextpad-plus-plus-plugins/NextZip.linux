/*
 * LinuxViewBridge.cpp — resolves the macOS-shaped plugin SDK onto the Linux
 * host. See NppPluginInterfaceLinux.h for the why.
 *
 * Host couplings, all in this one file (each mirrors something the macOS port
 * already does via KVC into MainWindowController — same fidelity class):
 *   - The editor notebooks are found by walking the widget tree from the main
 *     window for GtkNotebooks with the "npp-editor-tabs" CSS class; a notebook
 *     that is the END child of a GtkPaned is a secondary view (H-oriented
 *     paned => the vertical split = SUB_VIEW). (macOS: _tabManager /
 *     _subTabManagerV KVC.)
 *   - Each page is a GtkScrolledWindow whose child is the doc's ScintillaView;
 *     the owning buffer is the sci's "npp-doc" object data. On this host a
 *     document KEEPS its ScintillaView for life (unlike Windows/macOS where
 *     two shared views swap documents), which makes buffer->sci stable.
 *   - Moving a file to the other view / resetting = the host's own PUBLIC app
 *     actions "move-to-vview" / "reset-view" (what the View menu invokes).
 *     NEVER NPPM_MENUCOMMAND(10001): on this host that id lands in the
 *     plugin-command space (cmdIDs start at 10000) and would invoke a random
 *     plugin item.
 *
 * Linux port by Andrew Letov, 2026 (GPL v3, as upstream).
 */
#include "NppPluginInterfaceLinux.h"

#include <gtk/gtk.h>
#include <vector>

// The Scintilla widget entry point — resolves at dlopen from the host-loaded
// libscintilla.so (do NOT link libscintilla at build time).
extern "C" intptr_t scintilla_view_send_message(void* view, unsigned int msg,
                                                uintptr_t wParam, intptr_t lParam);

namespace
{

LinuxHostNppData g_host = {};

// CallScintilla(view) redirect targets (macOS EngineBridge gSciRedirect).
void* g_redirect[2] = { nullptr, nullptr };

// ── notebook discovery ──────────────────────────────────────────────────────

struct Notebooks
{
    GtkWidget* primary   = nullptr;
    GtkWidget* secondV   = nullptr;   // vertical split (right pane)  = SUB_VIEW
    GtkWidget* secondH   = nullptr;   // horizontal split (bottom pane)
};

// A notebook reached by being the END child of some ancestor GtkPaned is a
// secondary view; the paned's orientation tells which one.
static void classifyNotebook(GtkWidget* nb, Notebooks& out)
{
    for (GtkWidget* w = nb; w; )
    {
        GtkWidget* parent = gtk_widget_get_parent(w);
        if (parent && GTK_IS_PANED(parent) && gtk_paned_get_end_child(GTK_PANED(parent)) == w)
        {
            if (gtk_orientable_get_orientation(GTK_ORIENTABLE(parent)) == GTK_ORIENTATION_HORIZONTAL)
                out.secondV = nb;
            else
                out.secondH = nb;
            return;
        }
        w = parent;
    }
    if (!out.primary)
        out.primary = nb;
}

static void findNotebooksIn(GtkWidget* w, Notebooks& out)
{
    if (GTK_IS_NOTEBOOK(w) && gtk_widget_has_css_class(w, "npp-editor-tabs"))
        classifyNotebook(w, out);

    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        findNotebooksIn(c, out);
}

static Notebooks findNotebooks()
{
    Notebooks nbs;
    if (g_host.nppHandle)
        findNotebooksIn(g_host.nppHandle, nbs);
    return nbs;
}

// ── notebook cache ──────────────────────────────────────────────────────────
// findNotebooks() walks the host's ENTIRE widget tree. Uncached, every single
// CallScintilla paid that walk — a compare issues tens of thousands of per-line
// SCI calls, which turned an instant diff into seconds (measured: 1.86 s for a
// 1200-line file pair; ~85 ms cached). The cache holds GObject WEAK pointers,
// so a destroyed notebook nulls its slot automatically — a cached hit can
// never be a dangling widget. Rescan policy:
//   • primary NULL (first call / window rebuilt)     → rescan now;
//   • requested slot NULL (e.g. no split yet)        → rescan at most every
//     100 ms, so a no-split steady state doesn't re-walk per call but a
//     freshly-created split is still picked up promptly;
//   • cpHostMoveToOtherVerticalView / cpHostResetView → force next rescan
//     (we just changed the split ourselves).
// All bridge calls run on the GTK main thread (the engine's worker threads
// operate on pre-fetched data), so no locking is needed.
static Notebooks g_nbCache;
static gint64    g_nbScanTime = 0;   // monotonic µs of last scan; 0 = force

static void nbCacheDropWeaks()
{
    if (g_nbCache.primary) g_object_remove_weak_pointer(G_OBJECT(g_nbCache.primary), (gpointer*)&g_nbCache.primary);
    if (g_nbCache.secondV) g_object_remove_weak_pointer(G_OBJECT(g_nbCache.secondV), (gpointer*)&g_nbCache.secondV);
    if (g_nbCache.secondH) g_object_remove_weak_pointer(G_OBJECT(g_nbCache.secondH), (gpointer*)&g_nbCache.secondH);
    g_nbCache = Notebooks();
}

static void nbCacheRescan()
{
    nbCacheDropWeaks();
    g_nbCache = findNotebooks();
    if (g_nbCache.primary) g_object_add_weak_pointer(G_OBJECT(g_nbCache.primary), (gpointer*)&g_nbCache.primary);
    if (g_nbCache.secondV) g_object_add_weak_pointer(G_OBJECT(g_nbCache.secondV), (gpointer*)&g_nbCache.secondV);
    if (g_nbCache.secondH) g_object_add_weak_pointer(G_OBJECT(g_nbCache.secondH), (gpointer*)&g_nbCache.secondH);
    g_nbScanTime = g_get_monotonic_time();
}

static void nbCacheInvalidate()
{
    g_nbScanTime = 0;
}

// Current page's ScintillaView of a notebook (page = scrolled window -> sci).
static GtkWidget* notebookCurrentSci(GtkWidget* nb)
{
    if (!nb) return nullptr;
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(nb));
    if (page < 0) return nullptr;
    GtkWidget* sw = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), page);
    if (!sw || !GTK_IS_SCROLLED_WINDOW(sw)) return nullptr;
    return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(sw));
}

static GtkWidget* notebookSciAt(GtkWidget* nb, int page)
{
    if (!nb) return nullptr;
    GtkWidget* sw = gtk_notebook_get_nth_page(GTK_NOTEBOOK(nb), page);
    if (!sw || !GTK_IS_SCROLLED_WINDOW(sw)) return nullptr;
    return gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(sw));
}

// View id -> notebook. MAIN = primary, SUB = the VERTICAL split (ComparePlus
// only ever splits vertically — matching macOS moveToOtherVerticalView).
static GtkWidget* viewNotebook(int viewNum)
{
    GtkWidget** slot = (viewNum == SUB_VIEW) ? &g_nbCache.secondV : &g_nbCache.primary;
    if (!g_nbCache.primary ||
        (!*slot && g_get_monotonic_time() - g_nbScanTime > 100000))
        nbCacheRescan();
    return *slot;
}

// Resolved sci for CallScintilla(view): redirect wins; else the view's current
// tab; a missing SUB view falls back to the primary (mirrors the macOS host,
// which resolves the sub handle to the primary editor when there is no split).
static GtkWidget* resolveViewSci(int viewNum)
{
    if ((viewNum == 0 || viewNum == 1) && g_redirect[viewNum])
        return (GtkWidget*)g_redirect[viewNum];

    GtkWidget* sci = notebookCurrentSci(viewNotebook(viewNum));
    if (!sci && viewNum == SUB_VIEW)
        sci = notebookCurrentSci(viewNotebook(MAIN_VIEW));
    return sci;
}

static intptr_t buffIdOfSci(GtkWidget* sci)
{
    return sci ? (intptr_t)g_object_get_data(G_OBJECT(sci), "npp-doc") : 0;
}

} // anonymous namespace


// ── public bridge API ───────────────────────────────────────────────────────

void cpBridgeInit(const LinuxHostNppData* host)
{
    g_host = *host;
}

void* cpHostWindow()
{
    return g_host.nppHandle;
}

void setScintillaRedirect(int viewNum, void* sciWidget)
{
    if (viewNum == 0 || viewNum == 1)
        g_redirect[viewNum] = sciWidget;
}

void* cpViewSci(int viewNum)
{
    return notebookCurrentSci(viewNotebook(viewNum));
}

int cpViewCount(int viewNum)
{
    GtkWidget* nb = viewNotebook(viewNum);
    return nb ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) : 0;
}

int cpTotalFileCount()
{
    return cpViewCount(MAIN_VIEW) + cpViewCount(SUB_VIEW);
}

intptr_t cpCurrentViewId()
{
    // The host's "current" doc follows the focused notebook; ask it for the
    // current sci and see which view's current tab that is.
    GtkWidget* cur = (GtkWidget*)(intptr_t)g_host.hostMsg(NPPM_GETCURRENTSCINTILLA, 0, 0);
    GtkWidget* sub = notebookCurrentSci(viewNotebook(SUB_VIEW));
    return (cur && sub && cur == sub) ? SUB_VIEW : MAIN_VIEW;
}

void* cpBufferSci(intptr_t buffId)
{
    Notebooks nbs = findNotebooks();
    for (GtkWidget* nb : { nbs.primary, nbs.secondV, nbs.secondH })
    {
        if (!nb) continue;
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb));
        for (int i = 0; i < n; i++)
        {
            GtkWidget* sci = notebookSciAt(nb, i);
            if (sci && buffIdOfSci(sci) == buffId)
                return sci;
        }
    }
    return nullptr;
}

// ── host actions ────────────────────────────────────────────────────────────

static void activateAppAction(const char* name)
{
    GApplication* app = g_application_get_default();
    if (app)
        g_action_group_activate_action(G_ACTION_GROUP(app), name, nullptr);
}

void cpHostMoveToOtherVerticalView() { activateAppAction("move-to-vview"); nbCacheInvalidate(); }
void cpHostResetView()               { activateAppAction("reset-view");    nbCacheInvalidate(); }

// ── the routing _sendMessage ────────────────────────────────────────────────

intptr_t cpSendMessage(NppHandle handle, unsigned int msg, uintptr_t wParam, intptr_t lParam)
{
    // ── SCI messages to the view sentinels ──
    if (handle == kHandleScintillaMain || handle == kHandleScintillaSub)
    {
        const int viewNum = (handle == kHandleScintillaSub) ? SUB_VIEW : MAIN_VIEW;
        GtkWidget* sci = resolveViewSci(viewNum);
        return sci ? scintilla_view_send_message(sci, msg, wParam, lParam) : 0;
    }

    if (handle != kHandleNpp)
        return 0;

    // ── NPPM messages needing Windows dual-view semantics (the Linux host's
    //    handlers are single-view; these are answered from the notebooks) ──
    switch (msg)
    {
        case NPPM_GETCURRENTSCINTILLA:
        {
            // Contract: writes the focused VIEW ID (0/1) into *lParam and (on
            // this SDK shape) returns the sci — matching the macOS host.
            intptr_t view = cpCurrentViewId();
            if (lParam) *((int*)(intptr_t)lParam) = (int)view;
            return (intptr_t)notebookCurrentSci(viewNotebook((int)view));
        }

        case NPPM_GETCURRENTVIEW:
            return cpCurrentViewId();

        case NPPM_GETNBOPENFILES:
            // lParam: ALL_OPEN_FILES / PRIMARY_VIEW / SECOND_VIEW (Windows canon).
            if (lParam == PRIMARY_VIEW) return cpViewCount(MAIN_VIEW);
            if (lParam == SECOND_VIEW)  return cpViewCount(SUB_VIEW);
            return cpTotalFileCount();

        case NPPM_GETBUFFERIDFROMPOS:
        {
            // wParam = position, lParam = view.
            GtkWidget* sci = notebookSciAt(viewNotebook((int)lParam), (int)wParam);
            return buffIdOfSci(sci);
        }

        case NPPM_GETPOSFROMBUFFERID:
        {
            // Returns (view << 30) | position — the Windows packing the plugin
            // unpacks in viewIdFromBuffId()/posFromBuffId().
            Notebooks nbs = findNotebooks();
            const struct { GtkWidget* nb; intptr_t view; } views[] = {
                { nbs.primary, MAIN_VIEW }, { nbs.secondV, SUB_VIEW },
            };
            for (const auto& v : views)
            {
                if (!v.nb) continue;
                int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(v.nb));
                for (int i = 0; i < n; i++)
                    if (buffIdOfSci(notebookSciAt(v.nb, i)) == (intptr_t)wParam)
                        return (v.view << 30) | i;
            }
            return -1;
        }

        case NPPM_GETCURRENTDOCINDEX:
        {
            // lParam = view; index of that view's current tab.
            GtkWidget* nb = viewNotebook((int)lParam);
            return nb ? gtk_notebook_get_current_page(GTK_NOTEBOOK(nb)) : -1;
        }

        case NPPM_ACTIVATEDOC:
        {
            // wParam = view, lParam = index. Activate that tab AND focus its
            // sci so the host's focus-tracked "current view" follows (the
            // Windows host switches the active view too).
            GtkWidget* nb = viewNotebook((int)wParam);
            if (!nb) return 0;
            gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), (int)lParam);
            GtkWidget* sci = notebookCurrentSci(nb);
            if (sci) gtk_widget_grab_focus(sci);
            return 1;
        }

        default:
            // Everything else keeps the host's own semantics.
            return (intptr_t)g_host.hostMsg(msg, (unsigned long)wParam, (long)lParam);
    }
}
