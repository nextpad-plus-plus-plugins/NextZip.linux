/*
 * NextZipController.cpp — the NextZip archive File-Manager panel (GTK4).
 *
 * Browse an archive like a filesystem (folder tree built from entry paths),
 * breadcrumb + Up navigation, toolbar (Open/Up/Extract/Test/Info), and — the
 * point — open a file from the archive in the Nextpad++ editor by extracting it
 * on the fly to a temp folder, plus Extract/Test to disk. Linux port of the
 * macOS NextZipController; the plugin's stacked two-pane layout (filesystem
 * browser on top, archive contents below) is reproduced 1:1. The macOS
 * standalone-app extras (side-by-side layout, live filter, drag&drop) were
 * app-shell-only and are not part of the plugin on either platform.
 *
 * This file is host-agnostic: it never references NppData / NPPM_* directly.
 * Everything host-specific goes through m_host (NextZipHost).
 *
 * NextZip 2026 (GPL). Engine: 7-Zip (LGPL + unRAR restriction; RAR extract-only).
 */
#include "NextZipController.h"
#include "NextZipDialogs.h"
#include "SevenZipEngine.h"

#include <glib/gstdio.h>

#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <functional>
#include <cctype>
#include <map>
#include <set>
#include <utility>

// ── virtual folder tree built from the flat entry list (verbatim from macOS) ──
namespace {
struct FMNode {
	std::string name;
	bool        isDir = false;
	int         entryIndex = -1;          // index into engine.entries(), or -1 (synthetic dir)
	FMNode*     parent = nullptr;
	std::vector<FMNode*> children;
};

void freeTree(FMNode* n) { if (!n) return; for (auto c : n->children) freeTree(c); delete n; }

// A chain of nested-archive layers, outer → inner. .first = the archive file on
// disk (real for [0], a temp for unwrapped inner layers); .second = the single
// entry name inside that layer which yielded the next one (used to re-wrap on
// save). Length 1 for an ordinary archive; longer for .tar.gz etc.
using ArcChain = std::vector<std::pair<std::string, std::string>>;

// Which formats are single-stream compressors that wrap exactly one payload —
// the ones we transparently unwrap (e.g. site.tar.gz → show the inner tar).
bool isSingleStream(const std::string& fmt) {
	return fmt == "gzip" || fmt == "bzip2" || fmt == "xz" || fmt == "z";
}

// A file extracted to temp and opened in the editor → how to write it back.
struct OpenedTemp {
	std::string entryPath;   // path inside the innermost archive
	ArcChain    chain;       // layer chain captured at open time (for re-wrap)
};

std::vector<std::string> splitPath(const std::string& p) {
	std::vector<std::string> out; std::string cur;
	for (char c : p) {
		if (c == '/' || c == '\\') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
		else cur += c;
	}
	if (!cur.empty()) out.push_back(cur);
	return out;
}

FMNode* findOrAddDir(FMNode* p, const std::string& name) {
	for (auto c : p->children) if (c->isDir && c->name == name) return c;
	FMNode* n = new FMNode; n->isDir = true; n->name = name; n->parent = p;
	p->children.push_back(n); return n;
}

// Path of a node as a list of folder names from the root (root excluded). Used
// to re-resolve the current directory after the tree is freed + rebuilt.
std::vector<std::string> nodePath(FMNode* n) {
	std::vector<std::string> p;
	for (FMNode* c = n; c && c->parent; c = c->parent) p.push_back(c->name);
	std::reverse(p.begin(), p.end());
	return p;
}

// Find a directory node by its name-path in a (re)built tree; nullptr if any
// component is missing (e.g. the folder was removed).
FMNode* findDirByPath(FMNode* root, const std::vector<std::string>& path) {
	FMNode* cur = root;
	for (const std::string& name : path) {
		FMNode* next = nullptr;
		for (auto c : cur->children) if (c->isDir && c->name == name) { next = c; break; }
		if (!next) return nullptr;
		cur = next;
	}
	return cur;
}

FMNode* buildTree(const std::vector<NZEntry>& entries) {
	FMNode* root = new FMNode; root->isDir = true;
	for (int i = 0; i < (int)entries.size(); i++) {
		std::vector<std::string> comps = splitPath(entries[i].path);
		if (comps.empty()) continue;
		FMNode* cur = root;
		for (size_t k = 0; k + 1 < comps.size(); k++) cur = findOrAddDir(cur, comps[k]);
		const std::string& leaf = comps.back();
		if (entries[i].isDir) findOrAddDir(cur, leaf)->entryIndex = i;
		else { FMNode* f = new FMNode; f->name = leaf; f->entryIndex = i; f->parent = cur; cur->children.push_back(f); }
	}
	// sort each folder: dirs first, then case-insensitive name
	std::function<void(FMNode*)> sortNode = [&](FMNode* n) {
		std::sort(n->children.begin(), n->children.end(), [](FMNode* a, FMNode* b) {
			if (a->isDir != b->isDir) return a->isDir > b->isDir;
			std::string x = a->name, y = b->name;
			std::transform(x.begin(), x.end(), x.begin(), ::tolower);
			std::transform(y.begin(), y.end(), y.begin(), ::tolower);
			return x < y;
		});
		for (auto c : n->children) sortNode(c);
	};
	sortNode(root);
	return root;
}

void gatherEntryIndices(FMNode* n, std::vector<uint32_t>& out) {
	if (n->entryIndex >= 0) out.push_back((uint32_t)n->entryIndex);
	for (auto c : n->children) gatherEntryIndices(c, out);
}

std::string humanSize(uint64_t n) {
	if (n == 0) return "";
	static const char* u[] = {"B","KB","MB","GB","TB"};
	double v = (double)n; int i = 0;
	while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
	char buf[64];
	if (i == 0) snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
	else        snprintf(buf, sizeof(buf), "%.1f %s", v, u[i]);
	return buf;
}

bool pathIsDir(const std::string& p) {
	return g_file_test(p.c_str(), G_FILE_TEST_IS_DIR);
}
// Quick extension heuristic for building the right-click menu (the actual open is
// always content-detected). Folders are never archives.
bool looksLikeArchive(const std::string& path) {
	if (pathIsDir(path)) return false;
	static const std::set<std::string> exts = {
		"7z","zip","rar","tar","gz","tgz","bz2","tbz","tbz2","xz","txz","z","taz","cab",
		"iso","dmg","wim","swm","arj","lzh","lha","rpm","deb","xar","pkg","xip","cpio",
		"jar","war","apk","chm","udf","vhd","vhdx","vdi","vmdk","qcow","qcow2","cramfs","squashfs"};
	size_t dot = path.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string e = path.substr(dot + 1);
	std::transform(e.begin(), e.end(), e.begin(), ::tolower);
	return exts.count(e) > 0;
}

std::string sDirName(const std::string& p) {
	size_t sl = p.find_last_of('/');
	return sl == std::string::npos ? "" : (sl == 0 ? "/" : p.substr(0, sl));
}
std::string sBaseName(const std::string& p) {
	size_t sl = p.find_last_of('/');
	return sl == std::string::npos ? p : p.substr(sl + 1);
}
std::string sStripExt(const std::string& p) {
	size_t dot = p.find_last_of('.');
	return (dot == std::string::npos || dot == 0) ? p : p.substr(0, dot);
}

GIcon* iconForName(const std::string& name, bool isDir) {
	if (isDir) return g_themed_icon_new("folder");
	gboolean uncertain = FALSE;
	gchar* ct = g_content_type_guess(name.c_str(), nullptr, 0, &uncertain);
	GIcon* ic = ct ? g_content_type_get_icon(ct) : g_themed_icon_new("text-x-generic");
	g_free(ct);
	return ic;
}

// ── popover context menus with C++ lambda payloads (shared plugin pattern) ───
struct PopupMenu {
	GMenu* menu;
	GSimpleActionGroup* group;
	std::vector<std::function<void()>>* actions;
	int nextId = 0;
	GMenu* section;

	PopupMenu() {
		menu = g_menu_new();
		group = g_simple_action_group_new();
		actions = new std::vector<std::function<void()>>();
		section = g_menu_new();
	}
	void add(const std::string& label, std::function<void()> fn) {
		std::string name = "i" + std::to_string(nextId++);
		GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
		actions->push_back(std::move(fn));
		size_t idx = actions->size() - 1;
		g_object_set_data(G_OBJECT(a), "nz-idx", GSIZE_TO_POINTER(idx));
		g_signal_connect(a, "activate",
			G_CALLBACK(+[](GSimpleAction* act, GVariant*, gpointer u) {
				auto* fns = (std::vector<std::function<void()>>*)u;
				size_t i = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(act), "nz-idx"));
				if (i < fns->size() && (*fns)[i]) (*fns)[i]();
			}), actions);
		g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
		g_object_unref(a);
		g_menu_append(section, label.c_str(), ("nz." + name).c_str());
	}
	void separator() {
		g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
		g_object_unref(section);
		section = g_menu_new();
	}
	// Submenu of items sharing one handler parameterized by the label.
	void submenu(const std::string& label, const std::vector<std::string>& items,
	             std::function<void(const std::string&)> fn) {
		GMenu* sub = g_menu_new();
		for (const std::string& it : items) {
			std::string name = "i" + std::to_string(nextId++);
			GSimpleAction* a = g_simple_action_new(name.c_str(), nullptr);
			std::string arg = it;
			auto bound = fn;
			actions->push_back([bound, arg] { bound(arg); });
			size_t idx = actions->size() - 1;
			g_object_set_data(G_OBJECT(a), "nz-idx", GSIZE_TO_POINTER(idx));
			g_signal_connect(a, "activate",
				G_CALLBACK(+[](GSimpleAction* act, GVariant*, gpointer u) {
					auto* fns = (std::vector<std::function<void()>>*)u;
					size_t i = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(act), "nz-idx"));
					if (i < fns->size() && (*fns)[i]) (*fns)[i]();
				}), actions);
			g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
			g_object_unref(a);
			g_menu_append(sub, it.c_str(), ("nz." + name).c_str());
		}
		g_menu_append_submenu(section, label.c_str(), G_MENU_MODEL(sub));
		g_object_unref(sub);
	}
	void popupAt(GtkWidget* parent, double x, double y) {
		separator();   // flush the open section
		GtkWidget* pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
		gtk_widget_insert_action_group(pop, "nz", G_ACTION_GROUP(group));
		gtk_widget_set_parent(pop, parent);
		GdkRectangle r { (int)x, (int)y, 1, 1 };
		gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
		g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover* p, gpointer) {
			g_idle_add(+[](gpointer w) -> gboolean {
				gtk_widget_unparent(GTK_WIDGET(w));
				return G_SOURCE_REMOVE;
			}, p);
		}), nullptr);
		g_object_set_data_full(G_OBJECT(pop), "nz-fns", actions,
			+[](gpointer d) { delete (std::vector<std::function<void()>>*)d; });
		g_object_set_data_full(G_OBJECT(pop), "nz-menu", menu, g_object_unref);
		g_object_set_data_full(G_OBJECT(pop), "nz-group", group, g_object_unref);
		gtk_popover_popup(GTK_POPOVER(pop));
	}
};
} // namespace

// GtkTreeView is deprecated since GTK 4.10 but fully functional; the NppFTP
// port ships on it. Silence the noise for this translation unit.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

// ─────────────────────────────────────────────────────────────────────────────
struct NextZipController::Impl {
	NextZipController* self = nullptr;
	NextZipHost**      host = nullptr;         // points at controller's m_host

	std::unique_ptr<NextZipEngine> engine;
	FMNode*     root = nullptr;
	FMNode*     cwd = nullptr;
	std::vector<FMNode*> ancestors;            // root..cwd, parallel to breadcrumb items
	std::string archivePath;                   // innermost archive the engine is open on (may be a temp)
	std::string archivePassword;               // password that unlocked the on-screen archive
	bool        havePassword = false;
	std::string displayPath;                   // outermost real file the user clicked (breadcrumb)
	ArcChain    layers;                        // outer→inner nested-archive chain

	GtkWidget*  panel = nullptr;               // the archive-manager content view
	GtkWidget*  arcView = nullptr;             // bottom: archive contents GtkTreeView
	GtkListStore* arcStore = nullptr;
	GtkWidget*  breadcrumb = nullptr;          // GtkBox of ancestor buttons
	GtkWidget*  fsView = nullptr;              // top: filesystem GtkTreeView
	GtkTreeStore* fsStore = nullptr;

	// temp files opened in the editor → how to write them back (incl. nested chain)
	std::map<std::string, OpenedTemp> openedTemps;

	enum { FS_ICON, FS_NAME, FS_PATH, FS_ISDIR, FS_DUMMY, FS_NCOLS };
	enum { AR_ICON, AR_NAME, AR_SIZE, AR_PACK, AR_CRC, AR_METHOD, AR_MODIFIED, AR_NODE, AR_NCOLS };

	// ── panel construction ───────────────────────────────────────────────────
	GtkWidget* toolButton(const char* tip, const char* iconName, std::function<void()> fn) {
		GtkWidget* b = gtk_button_new_from_icon_name(iconName);
		gtk_widget_set_tooltip_text(b, tip);
		gtk_widget_add_css_class(b, "flat");
		auto* heap = new std::function<void()>(std::move(fn));
		g_object_set_data_full(G_OBJECT(b), "nz-fn", heap,
			+[](gpointer d) { delete (std::function<void()>*)d; });
		g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer) {
			auto* f = (std::function<void()>*)g_object_get_data(G_OBJECT(btn), "nz-fn");
			if (f && *f) (*f)();
		}), nullptr);
		return b;
	}

	void ensurePanel() {
		if (panel) return;
		panel = gtk_paned_new(GTK_ORIENTATION_VERTICAL);   // stacked: FS top, archive below
		gtk_widget_set_hexpand(panel, TRUE);
		gtk_widget_set_vexpand(panel, TRUE);
		g_object_set_data(G_OBJECT(panel), "nz-impl", this);

		// ── TOP pane: toolbar (Add/Extract/Test/Delete/Info) + filesystem browser ──
		GtkWidget* top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
		GtkWidget* fstb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_margin_start(fstb, 8); gtk_widget_set_margin_top(fstb, 6);
		gtk_box_append(GTK_BOX(fstb), toolButton("Add to archive…", "list-add-symbolic", [this]{ fsAdd(); }));
		gtk_box_append(GTK_BOX(fstb), toolButton("Extract…", "document-save-symbolic", [this]{ fsExtract(); }));
		gtk_box_append(GTK_BOX(fstb), toolButton("Test archive", "emblem-ok-symbolic", [this]{ fsTest(); }));
		gtk_box_append(GTK_BOX(fstb), toolButton("Delete (to Trash)", "user-trash-symbolic", [this]{ fsDelete(); }));
		gtk_box_append(GTK_BOX(fstb), toolButton("Info / Checksum", "dialog-information-symbolic", [this]{ fsInfo(); }));
		gtk_box_append(GTK_BOX(fstb), toolButton("Refresh", "view-refresh-symbolic", [this]{ refreshFs(); }));
		gtk_box_append(GTK_BOX(top), fstb);

		fsStore = gtk_tree_store_new(FS_NCOLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
		                             G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
		fsView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(fsStore));
		gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(fsView), FALSE);
		gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(fsView)),
		                            GTK_SELECTION_MULTIPLE);
		{
			// The host docks this panel via a GtkPaned whose separator carries
			// an ENLARGED invisible grab zone (~14px) that swallows clicks
			// along the panel's left edge — precisely where depth-1 expander
			// arrows would render (traced: presses at widget x≤12 never reach
			// this widget). A widget margin would fix reachability but leaves
			// a background strip and an inset selection highlight. Instead:
			// a narrow SPACER column plus making the name column the EXPANDER
			// column pushes the arrows right of the theft zone while row
			// backgrounds and the selection highlight still span the full
			// panel width.
			GtkTreeViewColumn* spacer = gtk_tree_view_column_new();
			gtk_tree_view_column_set_sizing(spacer, GTK_TREE_VIEW_COLUMN_FIXED);
			gtk_tree_view_column_set_fixed_width(spacer, 18);
			gtk_tree_view_append_column(GTK_TREE_VIEW(fsView), spacer);

			GtkTreeViewColumn* col = gtk_tree_view_column_new();
			GtkCellRenderer* ri = gtk_cell_renderer_pixbuf_new();
			GtkCellRenderer* rt = gtk_cell_renderer_text_new();
			gtk_tree_view_column_pack_start(col, ri, FALSE);
			gtk_tree_view_column_pack_start(col, rt, TRUE);
			gtk_tree_view_column_add_attribute(col, ri, "gicon", FS_ICON);
			gtk_tree_view_column_add_attribute(col, rt, "text", FS_NAME);
			gtk_tree_view_append_column(GTK_TREE_VIEW(fsView), col);
			gtk_tree_view_set_expander_column(GTK_TREE_VIEW(fsView), col);
		}
		// Populate on test-expand-row (BEFORE the view starts expanding), never on
		// row-expanded — see fsTestExpandRow for why.
		g_signal_connect(fsView, "test-expand-row",
			G_CALLBACK(+[](GtkTreeView*, GtkTreeIter* it, GtkTreePath*, gpointer u) -> gboolean {
				((Impl*)u)->fsTestExpandRow(it);
				return FALSE;   // FALSE = allow the expansion to proceed
			}), this);
		g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(fsView)), "changed",
			G_CALLBACK(+[](GtkTreeSelection* sel, gpointer u) { ((Impl*)u)->fsSelectionChanged(sel); }), this);
		g_signal_connect(fsView, "row-activated",
			G_CALLBACK(+[](GtkTreeView* tv, GtkTreePath* path, GtkTreeViewColumn*, gpointer) {
				// double-click a folder → expand/collapse (macOS onFsDoubleClick)
				if (gtk_tree_view_row_expanded(tv, path)) gtk_tree_view_collapse_row(tv, path);
				else gtk_tree_view_expand_row(tv, path, FALSE);
			}), nullptr);
		{
			// GtkTreeView's native expander hit area is only a few pixels wide
			// (measured: 1 of 9 positions across the visible arrow actually
			// toggled), which reads as "the arrows don't work". Make the WHOLE
			// expander gutter clickable: a capture-phase button-1 gesture that
			// claims clicks left of the name cell's content start —
			// gtk_tree_view_get_cell_area() excludes the expander gutter, so
			// cell_area.x IS the gutter's right edge, exact for any theme and
			// nesting depth — and toggles the row itself. Clicks on the icon or
			// name are not claimed and behave as before (select / open).
			GtkGesture* exp = gtk_gesture_click_new();
			gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(exp), GDK_BUTTON_PRIMARY);
			gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(exp), GTK_PHASE_CAPTURE);
			g_signal_connect(exp, "pressed",
				G_CALLBACK(+[](GtkGestureClick* g, int nPress, double x, double y, gpointer u) {
					Impl* d = (Impl*)u;
					GtkTreeView* tv = GTK_TREE_VIEW(d->fsView);
					int bx = 0; int by = 0;
					gtk_tree_view_convert_widget_to_bin_window_coords(tv, (int)x, (int)y, &bx, &by);
					GtkTreePath* path = nullptr;
					if (!gtk_tree_view_get_path_at_pos(tv, bx, by, &path, nullptr, nullptr, nullptr))
						return;
					// Everything left of the expander column's cell content —
					// the spacer column AND the expander gutter — is one wide
					// toggle target (get_cell_area excludes the arrow area, so
					// cell.x is its exact right edge).
					GdkRectangle cell;
					gtk_tree_view_get_cell_area(tv, path,
						gtk_tree_view_get_expander_column(tv), &cell);
					const bool inGutter = bx < cell.x;
					if (inGutter) {
						GtkTreeIter it;
						if (nPress == 1 &&
						    gtk_tree_model_get_iter(GTK_TREE_MODEL(d->fsStore), &it, path) &&
						    gtk_tree_model_iter_has_child(GTK_TREE_MODEL(d->fsStore), &it)) {
							if (gtk_tree_view_row_expanded(tv, path))
								gtk_tree_view_collapse_row(tv, path);
							else
								gtk_tree_view_expand_row(tv, path, FALSE);
						}
						// claim every gutter press (incl. n_press>1) so the native
						// narrow-expander handling can't double-toggle
						gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
					}
					gtk_tree_path_free(path);
				}), this);
			gtk_widget_add_controller(fsView, GTK_EVENT_CONTROLLER(exp));
		}
		{
			GtkGesture* rc = gtk_gesture_click_new();
			gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
			g_signal_connect(rc, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer u) {
				((Impl*)u)->fsContextMenu(x, y);
				gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
			}), this);
			gtk_widget_add_controller(fsView, GTK_EVENT_CONTROLLER(rc));
		}
		GtkWidget* fsScroll = gtk_scrolled_window_new();
		gtk_widget_set_vexpand(fsScroll, TRUE);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(fsScroll), fsView);
		gtk_box_append(GTK_BOX(top), fsScroll);
		gtk_paned_set_start_child(GTK_PANED(panel), top);
		gtk_paned_set_resize_start_child(GTK_PANED(panel), FALSE);
		gtk_paned_set_shrink_start_child(GTK_PANED(panel), FALSE);

		fsPopulateRoots();

		// ── BOTTOM pane: breadcrumb ABOVE the toolbar, then the table ──
		GtkWidget* arc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

		breadcrumb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		gtk_widget_set_margin_start(breadcrumb, 8); gtk_widget_set_margin_top(breadcrumb, 5);
		gtk_box_append(GTK_BOX(arc), breadcrumb);

		GtkWidget* tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		gtk_widget_set_margin_start(tb, 8);
		gtk_box_append(GTK_BOX(tb), toolButton("Open archive…", "document-open-symbolic", [this]{ showOpenPanel(); }));
		gtk_box_append(GTK_BOX(tb), toolButton("Up", "go-up-symbolic", [this]{ actUp(); }));
		gtk_box_append(GTK_BOX(tb), toolButton("Extract…", "document-save-symbolic", [this]{ actExtract(); }));
		gtk_box_append(GTK_BOX(tb), toolButton("Test", "emblem-ok-symbolic", [this]{ actTest(); }));
		gtk_box_append(GTK_BOX(tb), toolButton("Info", "dialog-information-symbolic", [this]{ actInfo(); }));
		gtk_box_append(GTK_BOX(tb), toolButton("Refresh", "view-refresh-symbolic", [this]{ actRefresh(); }));
		gtk_box_append(GTK_BOX(arc), tb);

		arcStore = gtk_list_store_new(AR_NCOLS, G_TYPE_ICON, G_TYPE_STRING, G_TYPE_STRING,
		                              G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
		                              G_TYPE_POINTER);
		arcView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(arcStore));
		gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(arcView)),
		                            GTK_SELECTION_MULTIPLE);
		struct { const char* title; int col; int width; bool icon; } cols[] = {
			{"Name", AR_NAME, 340, true}, {"Size", AR_SIZE, 90, false}, {"Packed", AR_PACK, 90, false},
			{"CRC", AR_CRC, 80, false}, {"Method", AR_METHOD, 110, false}, {"Modified", AR_MODIFIED, 140, false},
		};
		for (auto& c : cols) {
			GtkTreeViewColumn* col = gtk_tree_view_column_new();
			gtk_tree_view_column_set_title(col, c.title);
			gtk_tree_view_column_set_resizable(col, TRUE);
			gtk_tree_view_column_set_fixed_width(col, c.width);
			if (c.icon) {
				GtkCellRenderer* ri = gtk_cell_renderer_pixbuf_new();
				gtk_tree_view_column_pack_start(col, ri, FALSE);
				gtk_tree_view_column_add_attribute(col, ri, "gicon", AR_ICON);
			}
			GtkCellRenderer* rt = gtk_cell_renderer_text_new();
			gtk_tree_view_column_pack_start(col, rt, TRUE);
			gtk_tree_view_column_add_attribute(col, rt, "text", c.col);
			gtk_tree_view_append_column(GTK_TREE_VIEW(arcView), col);
		}
		g_signal_connect(arcView, "row-activated",
			G_CALLBACK(+[](GtkTreeView*, GtkTreePath* path, GtkTreeViewColumn*, gpointer u) {
				((Impl*)u)->arcRowActivated(path);
			}), this);
		{
			GtkGesture* rc = gtk_gesture_click_new();
			gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
			g_signal_connect(rc, "pressed", G_CALLBACK(+[](GtkGestureClick* g, int, double x, double y, gpointer u) {
				((Impl*)u)->arcContextMenu(x, y);
				gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
			}), this);
			gtk_widget_add_controller(arcView, GTK_EVENT_CONTROLLER(rc));
		}
		GtkWidget* sc = gtk_scrolled_window_new();
		gtk_widget_set_vexpand(sc, TRUE);
		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), arcView);
		gtk_box_append(GTK_BOX(arc), sc);
		gtk_paned_set_end_child(GTK_PANED(panel), arc);

		gtk_paned_set_position(GTK_PANED(panel), 200);   // ~200px FS pane on top (plugin layout)
	}

	// ── top pane: filesystem browser (lazy GtkTreeStore, placeholder children) ──
	void fsAddPlaceholder(GtkTreeIter* parent) {
		GtkTreeIter dummy;
		gtk_tree_store_append(fsStore, &dummy, parent);
		gtk_tree_store_set(fsStore, &dummy, FS_NAME, "…", FS_PATH, "", FS_ISDIR, FALSE, FS_DUMMY, TRUE, -1);
	}
	void fsAppend(GtkTreeIter* parent, const std::string& path, const std::string& name, bool isDir) {
		GtkTreeIter it;
		gtk_tree_store_append(fsStore, &it, parent);
		GIcon* ic = iconForName(name, isDir);
		gtk_tree_store_set(fsStore, &it, FS_ICON, ic, FS_NAME, name.c_str(),
		                   FS_PATH, path.c_str(), FS_ISDIR, isDir ? TRUE : FALSE, FS_DUMMY, FALSE, -1);
		g_object_unref(ic);
		if (isDir) fsAddPlaceholder(&it);
	}
	void fsPopulateRoots() {
		gtk_tree_store_clear(fsStore);
		const char* home = g_get_home_dir();
		if (home) fsAppend(nullptr, home, home, true);
		fsAppend(nullptr, "/", "/", true);
	}
	// dirs first, then case-insensitive name; hidden files skipped (macOS parity)
	void fsChildrenOf(const std::string& dir, std::vector<std::pair<std::string, bool>>& out) {
		GDir* gd = g_dir_open(dir.c_str(), 0, nullptr);
		if (!gd) return;
		const gchar* f;
		while ((f = g_dir_read_name(gd)) != nullptr) {
			if (f[0] == '.') continue;
			std::string full = (dir == "/") ? ("/" + std::string(f)) : (dir + "/" + f);
			out.push_back({f, pathIsDir(full)});
		}
		g_dir_close(gd);
		std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
			if (a.second != b.second) return a.second > b.second;
			return g_ascii_strcasecmp(a.first.c_str(), b.first.c_str()) < 0;
		});
	}
	// Lazy-load a directory's children. MUST run on "test-expand-row", i.e.
	// BEFORE the view expands, and MUST append the real rows BEFORE dropping the
	// placeholder:
	//   • Populating from "row-expanded" mutates the model while GtkTreeView is
	//     mid-expand. Removing the placeholder first left the row with ZERO
	//     children for an instant, so the view abandoned the expansion (the
	//     arrow simply did not open) and the real children landed in a row it
	//     had already marked collapsed. That mid-expand mutation is also the
	//     likeliest source of the gtk_css_node_insert_after criticals.
	//   • Doing it here means all model changes are finished before the view
	//     starts, and the row never drops to zero children.
	// gtk_tree_view_expand_row() emits this synchronously, so fsExpandToPath's
	// walk still sees populated children immediately.
	void fsTestExpandRow(GtkTreeIter* it) {
		// only populate when the placeholder child is still present
		GtkTreeIter child;
		if (!gtk_tree_model_iter_children(GTK_TREE_MODEL(fsStore), &child, it)) return;
		gboolean dummy = FALSE;
		gtk_tree_model_get(GTK_TREE_MODEL(fsStore), &child, FS_DUMMY, &dummy, -1);
		if (!dummy) return;

		gchar* dirC = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(fsStore), it, FS_PATH, &dirC, -1);
		std::string dir = dirC ? dirC : ""; g_free(dirC);
		std::vector<std::pair<std::string, bool>> kids;
		fsChildrenOf(dir, kids);
		for (auto& k : kids) {
			std::string full = (dir == "/") ? ("/" + k.first) : (dir + "/" + k.first);
			fsAppend(it, full, k.first, k.second);   // append REAL rows first…
		}
		// …then drop the placeholder, so the row never has zero children.
		// (GtkTreeStore iters are persistent, so `child` is still valid here.)
		gtk_tree_store_remove(fsStore, &child);
	}
	// Single left-click on a file in the top pane → view its contents below.
	void fsSelectionChanged(GtkTreeSelection* sel) {
		if (gtk_tree_selection_count_selected_rows(sel) != 1) return;
		std::vector<std::string> p = selectedFsPaths();
		if (p.size() != 1) return;
		const std::string& path = p[0];
		if (pathIsDir(path)) return;                       // folder selection: do nothing
		if (!archivePath.empty() && path == displayPath) return;  // already shown
		self->openArchiveAtPath(path, /*quiet=*/true);     // non-archives are ignored quietly
	}
	std::vector<std::string> selectedFsPaths() {
		std::vector<std::string> out;
		GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(fsView));
		GtkTreeModel* model = nullptr;
		GList* rows = gtk_tree_selection_get_selected_rows(sel, &model);
		for (GList* l = rows; l; l = l->next) {
			GtkTreeIter it;
			if (gtk_tree_model_get_iter(model, &it, (GtkTreePath*)l->data)) {
				gchar* p = nullptr; gboolean dummy = FALSE;
				gtk_tree_model_get(model, &it, FS_PATH, &p, FS_DUMMY, &dummy, -1);
				if (p && *p && !dummy) out.push_back(p);
				g_free(p);
			}
		}
		g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
		return out;
	}
	std::string singleFsSelection() {
		auto p = selectedFsPaths();
		return p.size() == 1 ? p[0] : std::string();
	}
	std::string singleArchiveSelection() {
		std::string p = singleFsSelection();
		return (!p.empty() && !pathIsDir(p)) ? p : std::string();
	}
	// Clear + reload the FS tree, preserving expanded rows by path.
	void refreshFs() {
		std::vector<std::string> expanded;
		gtk_tree_view_map_expanded_rows(GTK_TREE_VIEW(fsView),
			+[](GtkTreeView* tv, GtkTreePath* path, gpointer u) {
				auto* exp = (std::vector<std::string>*)u;
				GtkTreeIter it;
				GtkTreeModel* m = gtk_tree_view_get_model(tv);
				if (gtk_tree_model_get_iter(m, &it, path)) {
					gchar* p = nullptr;
					gtk_tree_model_get(m, &it, FS_PATH, &p, -1);
					if (p && *p) exp->push_back(p);
					g_free(p);
				}
			}, &expanded);
		fsPopulateRoots();
		for (const std::string& p : expanded) fsExpandToPath(p, /*select=*/false);
	}
	// Expand the FS tree down to `target` (a directory); optionally select it.
	bool fsIterForPathIn(GtkTreeIter* parent, const std::string& target, GtkTreeIter* out) {
		GtkTreeIter it;
		gboolean ok = parent
			? gtk_tree_model_iter_children(GTK_TREE_MODEL(fsStore), &it, parent)
			: gtk_tree_model_get_iter_first(GTK_TREE_MODEL(fsStore), &it);
		while (ok) {
			gchar* p = nullptr;
			gtk_tree_model_get(GTK_TREE_MODEL(fsStore), &it, FS_PATH, &p, -1);
			std::string sp = p ? p : ""; g_free(p);
			if (sp == target) { *out = it; return true; }
			ok = gtk_tree_model_iter_next(GTK_TREE_MODEL(fsStore), &it);
		}
		return false;
	}
	void fsExpandToPath(const std::string& target, bool select) {
		// pick the root that prefixes target (home preferred, then "/")
		std::string rootPath;
		const char* home = g_get_home_dir();
		if (home && (target == home || target.rfind(std::string(home) + "/", 0) == 0)) rootPath = home;
		else rootPath = "/";
		// component chain from root → target
		std::vector<std::string> chain;
		std::string p = target;
		while (!p.empty() && p != rootPath) {
			chain.insert(chain.begin(), p);
			std::string parent = sDirName(p);
			if (parent.empty() || parent == p) break;
			p = parent;
		}
		GtkTreeIter cur;
		if (!fsIterForPathIn(nullptr, rootPath, &cur)) return;
		GtkTreePath* tp = gtk_tree_model_get_path(GTK_TREE_MODEL(fsStore), &cur);
		gtk_tree_view_expand_row(GTK_TREE_VIEW(fsView), tp, FALSE);
		gtk_tree_path_free(tp);
		for (const std::string& step : chain) {
			// row-expanded populated cur's children synchronously
			GtkTreeIter next;
			if (!fsIterForPathIn(&cur, step, &next)) break;
			cur = next;
			GtkTreePath* sp2 = gtk_tree_model_get_path(GTK_TREE_MODEL(fsStore), &cur);
			gtk_tree_view_expand_row(GTK_TREE_VIEW(fsView), sp2, FALSE);
			if (select && step == target) {
				gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(fsView)), sp2);
				gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(fsView), sp2, nullptr, FALSE, 0, 0);
			}
			gtk_tree_path_free(sp2);
		}
	}

	// ── open ─────────────────────────────────────────────────────────────────
	void showOpenPanel() {
		std::string p = NextZipDialogs::chooseOpenFile("");
		if (!p.empty()) self->openArchiveAtPath(p);
	}

	std::string newLayerTempDir() {
		std::string base = std::string(g_get_tmp_dir()) + "/NextZip/layers";
		g_mkdir_with_parents(base.c_str(), 0700);
		gchar* dir = g_strdup_printf("%s/layer-XXXXXX", base.c_str());
		gchar* made = g_mkdtemp(dir);
		std::string out = made ? made : base;
		g_free(dir);
		return out;
	}

	void openArchive(const std::string& path, bool quiet) {
		ensurePanel();
		if (!engine->open(path)) {
			if (!quiet) NextZipDialogs::alert("Could not open archive", engine->error());
			return;
		}
		displayPath = path;
		archivePassword.clear(); havePassword = false;   // a different archive → forget the cached password
		layers.clear();
		layers.push_back({path, std::string()});

		// Transparently descend single-stream compressors whose lone payload is
		// itself an archive: site.tar.gz → gzip{site.tar} → show the inner tar's
		// files. Only .gz/.bz2/.xz/.z (always exactly one payload); a 1-entry .zip
		// is left as-is. Stop when the payload isn't itself an archive.
		for (int guard = 0; guard < 6; guard++) {
			if (!isSingleStream(engine->format())) break;
			if (engine->entries().size() != 1 || engine->entries()[0].isDir) break;
			const std::string childName = engine->entries()[0].path;
			std::string tmpDir = newLayerTempDir();
			if (!engine->extract({0}, tmpDir)) break;
			// The compressor wrote exactly one file into tmpDir; take it regardless of name.
			GDir* gd = g_dir_open(tmpDir.c_str(), 0, nullptr);
			std::string inner;
			int count = 0;
			if (gd) {
				const gchar* f;
				while ((f = g_dir_read_name(gd)) != nullptr) { count++; inner = tmpDir + "/" + f; }
				g_dir_close(gd);
			}
			if (count != 1) break;
			std::unique_ptr<NextZipEngine> probe(new NextZipEngine());
			if (!probe->open(inner)) break;                 // plain file → keep the single entry
			layers.back().second = childName;               // remember for re-wrap on save
			layers.push_back({inner, std::string()});
			engine = std::move(probe);
		}

		archivePath = layers.back().first;
		freeTree(root);
		root = buildTree(engine->entries());
		navigateTo(root);
		if (*host) (*host)->revealPanel();
	}

	// ── navigation ───────────────────────────────────────────────────────────
	void navigateTo(FMNode* node) {
		if (!node) return;
		cwd = node;
		// ancestors root..cwd
		ancestors.clear();
		for (FMNode* n = node; n; n = n->parent) ancestors.insert(ancestors.begin(), n);
		// breadcrumb buttons
		GtkWidget* child;
		while ((child = gtk_widget_get_first_child(breadcrumb)) != nullptr)
			gtk_box_remove(GTK_BOX(breadcrumb), child);
		for (size_t i = 0; i < ancestors.size(); i++) {
			if (i) gtk_box_append(GTK_BOX(breadcrumb), gtk_label_new("›"));
			std::string title = (i == 0)
				? sBaseName(!displayPath.empty() ? displayPath : archivePath)
				: ancestors[i]->name;
			GtkWidget* b = gtk_button_new_with_label(title.c_str());
			gtk_widget_add_css_class(b, "flat");
			g_object_set_data(G_OBJECT(b), "nz-impl", this);
			g_object_set_data(G_OBJECT(b), "nz-idx", GSIZE_TO_POINTER(i));
			g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton* btn, gpointer) {
				Impl* d = (Impl*)g_object_get_data(G_OBJECT(btn), "nz-impl");
				size_t idx = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(btn), "nz-idx"));
				if (idx < d->ancestors.size()) d->navigateTo(d->ancestors[idx]);
			}), nullptr);
			gtk_box_append(GTK_BOX(breadcrumb), b);
		}
		reloadArcRows();
	}
	void actUp() { if (cwd && cwd->parent) navigateTo(cwd->parent); }

	void reloadArcRows() {
		gtk_list_store_clear(arcStore);
		if (!cwd) return;
		const std::vector<NZEntry>& E = engine->entries();
		for (FMNode* n : cwd->children) {
			const NZEntry* e = (n->entryIndex >= 0 && n->entryIndex < (int)E.size()) ? &E[n->entryIndex] : nullptr;
			GtkTreeIter it;
			gtk_list_store_append(arcStore, &it);
			GIcon* ic = iconForName(n->name, n->isDir);
			std::string size, pack, crc, method, modified;
			if (!n->isDir && e) {
				size = humanSize(e->size);
				pack = humanSize(e->packSize);
				if (e->hasCrc) { char b[16]; snprintf(b, sizeof(b), "%08X", e->crc); crc = b; }
				method = e->method;
				if (e->mtime != 0) {
					GDateTime* dt = g_date_time_new_from_unix_local(e->mtime);
					if (dt) {
						gchar* s = g_date_time_format(dt, "%Y-%m-%d %H:%M");
						modified = s ? s : ""; g_free(s);
						g_date_time_unref(dt);
					}
				}
			}
			gtk_list_store_set(arcStore, &it,
				AR_ICON, ic, AR_NAME, n->name.c_str(), AR_SIZE, size.c_str(),
				AR_PACK, pack.c_str(), AR_CRC, crc.c_str(), AR_METHOD, method.c_str(),
				AR_MODIFIED, modified.c_str(), AR_NODE, (gpointer)n, -1);
			g_object_unref(ic);
		}
		gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(arcView)));
	}

	FMNode* nodeAtTreePath(GtkTreePath* path) {
		GtkTreeIter it;
		if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(arcStore), &it, path)) return nullptr;
		gpointer p = nullptr;
		gtk_tree_model_get(GTK_TREE_MODEL(arcStore), &it, AR_NODE, &p, -1);
		return (FMNode*)p;
	}
	void arcRowActivated(GtkTreePath* path) {
		FMNode* n = nodeAtTreePath(path);
		if (!n) return;
		if (n->isDir) { navigateTo(n); return; }
		openEntryInEditor(n);
	}
	std::vector<uint32_t> selectedIndices() {
		std::vector<uint32_t> idx;
		GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(arcView));
		GtkTreeModel* model = nullptr;
		GList* rows = gtk_tree_selection_get_selected_rows(sel, &model);
		for (GList* l = rows; l; l = l->next) {
			GtkTreeIter it;
			if (gtk_tree_model_get_iter(model, &it, (GtkTreePath*)l->data)) {
				gpointer p = nullptr;
				gtk_tree_model_get(model, &it, AR_NODE, &p, -1);
				if (p) gatherEntryIndices((FMNode*)p, idx);
			}
		}
		g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
		return idx;
	}
	FMNode* selectedArcNode() {
		GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(arcView));
		GtkTreeModel* model = nullptr;
		GList* rows = gtk_tree_selection_get_selected_rows(sel, &model);
		FMNode* n = nullptr;
		if (rows) {
			GtkTreeIter it;
			if (gtk_tree_model_get_iter(model, &it, (GtkTreePath*)rows->data)) {
				gpointer p = nullptr;
				gtk_tree_model_get(model, &it, AR_NODE, &p, -1);
				n = (FMNode*)p;
			}
		}
		g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
		return n;
	}

	// ── password-aware extract / test (Windows 7-Zip behaviour) ──────────────
	bool extractWithEngine(NextZipEngine* eng, const std::vector<uint32_t>& indices,
	                       const std::string& dest, const std::string& initialPw,
	                       bool flatten, int overwrite, bool elim,
	                       const std::string& name, std::string* outPw) {
		std::string pw = initialPw;
		bool retried = !initialPw.empty();
		for (;;) {
			if (eng->extract(indices, dest, pw, flatten, overwrite, elim)) {
				if (outPw) *outPw = pw;
				return true;
			}
			if (!eng->lastErrorNeedsPassword()) {
				NextZipDialogs::alert("Extract failed", eng->error());
				return false;
			}
			std::string entered;
			if (!NextZipDialogs::promptPasswordForArchive(name, retried, entered))
				return false;           // user cancelled
			pw = entered;
			retried = true;
		}
	}
	bool testWithEngine(NextZipEngine* eng, const std::vector<uint32_t>& indices,
	                    const std::string& initialPw, const std::string& name, std::string* outPw) {
		std::string pw = initialPw;
		bool retried = !initialPw.empty();
		for (;;) {
			if (eng->test(indices, pw)) {
				if (outPw) *outPw = pw;
				return true;
			}
			if (!eng->lastErrorNeedsPassword()) return false;   // caller shows eng->error()
			std::string entered;
			if (!NextZipDialogs::promptPasswordForArchive(name, retried, entered))
				return false;           // user cancelled
			pw = entered;
			retried = true;
		}
	}

	// ── open a file from the archive in the editor (extract on the fly) ──────
	void openEntryInEditor(FMNode* n) {
		if (n->entryIndex < 0) return;
		std::string base = std::string(g_get_tmp_dir()) + "/NextZip/" + sBaseName(archivePath);
		std::vector<uint32_t> one{ (uint32_t)n->entryIndex };
		std::string usedPw;
		if (!extractWithEngine(engine.get(), one, base, havePassword ? archivePassword : "",
		                       false, 0, false, sBaseName(archivePath), &usedPw))
			return;   // alert already shown, or user cancelled the password prompt
		archivePassword = usedPw; havePassword = !usedPw.empty();
		std::string rel  = engine->entries()[n->entryIndex].path;
		std::string full = base + "/" + rel;
		// Record this temp ↔ archive entry (incl. the nested-archive chain) FIRST,
		// then hand the host a pointer into the map node (stable for the map's life).
		OpenedTemp ot{ engine->entries()[n->entryIndex].path, layers };
		auto res = openedTemps.insert_or_assign(full, std::move(ot));
		const char* stablePath = res.first->first.c_str();
		if (*host) (*host)->openExtractedFile(full, stablePath);
	}

	// ── save-back: editor saved a file we extracted → write it into the archive ─
	void handleFileSaved(const std::string& savedPath) {
		auto it = openedTemps.find(savedPath);
		if (it == openedTemps.end()) return;                 // not one of ours
		const std::string entry = it->second.entryPath;
		const ArcChain& chain   = it->second.chain;
		if (chain.empty()) return;

		std::string err;
		if (!rewriteChain(chain, entry, savedPath, err)) {
			NextZipDialogs::alert("Could not save back to archive", err);
			return;
		}
		g_message("[NextZip] saved '%s' back into %s", entry.c_str(), chain.front().first.c_str());
		// If what we just rewrote is the archive currently on screen, refresh the view.
		if (!archivePath.empty() && chain.back().first == archivePath && root) {
			// Capture the current folder by PATH first: cwd points into the tree we
			// are about to free. Re-resolve the same path in the rebuilt tree.
			std::vector<std::string> cwdPath = cwd ? nodePath(cwd) : std::vector<std::string>();
			engine->open(archivePath);
			freeTree(root); root = buildTree(engine->entries());
			FMNode* dest = findDirByPath(root, cwdPath);
			navigateTo(dest ? dest : root);
		}
	}

	// Write `entry` (= contents of `srcFile`) into chain.back(), then re-wrap each
	// parent layer from inner to outer, with throwaway engines.
	bool rewriteChain(const ArcChain& chain, const std::string& entry,
	                  const std::string& srcFile, std::string& err) {
		{
			NextZipEngine inner;
			if (!inner.open(chain.back().first) || !inner.updateFile(entry, srcFile)) {
				err = inner.error().empty() ? "could not open innermost archive" : inner.error();
				return false;
			}
		}
		for (long i = (long)chain.size() - 2; i >= 0; i--) {
			const std::string& parent    = chain[(size_t)i].first;
			const std::string& childName = chain[(size_t)i].second;
			const std::string& childFile = chain[(size_t)i + 1].first;
			NextZipEngine e;
			if (!e.open(parent) || !e.updateFile(childName, childFile)) {
				err = e.error().empty() ? "could not re-wrap nested archive" : e.error();
				return false;
			}
		}
		return true;
	}
	bool rewrapOutwardFromInner(std::string& err) {
		for (long i = (long)layers.size() - 2; i >= 0; i--) {
			NextZipEngine e;
			if (!e.open(layers[(size_t)i].first) ||
			    !e.updateFile(layers[(size_t)i].second, layers[(size_t)i + 1].first)) {
				err = e.error().empty() ? "re-wrap failed" : e.error();
				return false;
			}
		}
		return true;
	}

	// ═══════════════════════════════════════════════════════════════════════
	// TOP-PANE actions: Add / Extract / Test / Delete / Info
	// ═══════════════════════════════════════════════════════════════════════
	void fsAdd() {
		std::vector<std::string> inputs = selectedFsPaths();
		if (inputs.empty()) { NextZipDialogs::alert("Add to Archive", "Select one or more files or folders first."); return; }
		NZAddOptions o = NextZipDialogs::runAddForInputs(inputs);
		if (!o.ok) return;
		performAdd(o, inputs);
	}
	void fsAddQuick(const std::string& fmt) {
		std::vector<std::string> inputs = selectedFsPaths();
		if (inputs.empty()) return;
		const std::string& first = inputs.front();
		std::string dir = sDirName(first);
		std::string base = inputs.size() == 1 ? sStripExt(sBaseName(first)) : sBaseName(dir);
		if (base.empty()) base = "Archive";
		NZAddOptions o;
		o.ok = true;
		o.archivePath = dir + "/" + base + "." + fmt;
		o.format = fmt; o.level = 5;
		performAdd(o, inputs);
	}
	void performAdd(const NZAddOptions& o, const std::vector<std::string>& inputs) {
		if (o.archivePath.empty() || inputs.empty()) return;
		if (o.createSFX)
			NextZipDialogs::alert("Add to Archive", "SFX archives aren’t supported in the Linux build — that option was ignored.");
		if (!o.splitVolume.empty())
			NextZipDialogs::alert("Add to Archive", "Splitting to volumes isn’t supported yet — that value was ignored.");
		if (g_file_test(o.archivePath.c_str(), G_FILE_TEST_EXISTS)) {
			if (!NextZipDialogs::confirm("Overwrite existing archive?",
					o.archivePath + "\n\n(Adding into an existing archive is not yet supported.)",
					"Overwrite", "Cancel"))
				return;
		}
		NextZipEngine::CompressOptions opt;
		opt.format        = o.format;
		opt.level         = o.level;
		opt.method        = o.method;
		opt.dict          = o.dict;
		opt.wordSize      = (uint32_t)o.wordSize;
		opt.solid         = o.solid;
		opt.threads       = o.threads;
		opt.memusePercent = o.memusePercent;
		opt.password      = o.password;
		opt.encMethod     = o.encMethod;
		opt.encryptNames  = o.encryptNames;
		opt.pathMode      = o.pathMode;
		opt.deleteAfter   = o.deleteAfter;
		opt.extraParams   = o.extraParams;
		bool ok = engine->compress(o.archivePath, opt, inputs);
		if (ok) { refreshFs(); NextZipDialogs::alert("Add to Archive", "Created:\n" + o.archivePath); }
		else    NextZipDialogs::alert("Add to Archive failed", engine->error());
	}

	// Folder name for the "extract into subfolder" variants: strip one extension,
	// and for .tar.<z> doubles strip the inner .tar too (site.tar.gz → site;
	// notes.tgz already reduces to notes).
	static std::string nzExtractFolderName(const std::string& archivePath) {
		std::string name = sBaseName(archivePath);
		std::string base = sStripExt(name);
		size_t dot = base.find_last_of('.');
		if (dot != std::string::npos && dot > 0 && g_ascii_strcasecmp(base.c_str() + dot + 1, "tar") == 0)
			base = base.substr(0, dot);
		return base.empty() ? name : base;
	}
	void fsExtract() {
		std::string a = singleArchiveSelection();
		if (a.empty()) { NextZipDialogs::alert("Extract", "Select a single archive file first."); return; }
		NZExtractOptions o = NextZipDialogs::runExtractForArchive(a);
		if (!o.ok) return;
		std::string dest = o.destDir;
		if (o.intoSubfolder) dest += "/" + nzExtractFolderName(a);
		extractArchive(a, dest, o.password, o.pathMode == 1, o.overwrite, o.eliminateRoot);
	}
	void fsExtractHere() {
		std::string a = singleArchiveSelection(); if (a.empty()) return;
		extractArchive(a, sDirName(a), "", false, 0, false);
	}
	void fsExtractToSub() {
		std::string a = singleArchiveSelection(); if (a.empty()) return;
		std::string sub = sDirName(a) + "/" + nzExtractFolderName(a);
		extractArchive(a, sub, "", false, 0, false);
	}
	void extractArchive(const std::string& arcPath, const std::string& dest, const std::string& pw,
	                    bool flatten, int overwrite, bool elim) {
		if (arcPath.empty() || dest.empty()) return;
		std::unique_ptr<NextZipEngine> eng(new NextZipEngine());   // fresh engine — don't disturb the open view
		if (!eng->open(arcPath)) { NextZipDialogs::alert("Extract failed", eng->error()); return; }
		// Tarball descent, GATED to the .tar.gz/.tgz family: only when a
		// single-stream wrapper's lone payload is a TAR do we extract the tar's
		// files instead of the bare inner file. Anything else keeps the plain
		// wrapper behavior — data.json.gz yields data.json, archive.zip.gz yields
		// archive.zip (not the zip's contents).
		for (int guard = 0; guard < 6; guard++) {
			if (!isSingleStream(eng->format())) break;
			if (eng->entries().size() != 1 || eng->entries()[0].isDir) break;
			std::string tmpDir = newLayerTempDir();
			if (!eng->extract({0}, tmpDir)) break;
			// The compressor wrote exactly one file into tmpDir; take it regardless of name.
			GDir* gd = g_dir_open(tmpDir.c_str(), 0, nullptr);
			std::string inner;
			int count = 0;
			if (gd) {
				const gchar* f;
				while ((f = g_dir_read_name(gd)) != nullptr) { count++; inner = tmpDir + "/" + f; }
				g_dir_close(gd);
			}
			if (count != 1) break;
			std::unique_ptr<NextZipEngine> probe(new NextZipEngine());
			if (!probe->open(inner) || probe->format() != "tar")
				break;                                                  // not a tarball → extract the wrapper as-is
			eng = std::move(probe);
		}
		std::vector<uint32_t> all;                          // empty = everything
		bool ok = extractWithEngine(eng.get(), all, dest, pw, flatten, overwrite, elim,
		                            sBaseName(arcPath), nullptr);
		refreshFs();
		if (ok) NextZipDialogs::alert("Extract", "Extracted to:\n" + dest);
	}

	void fsTest() {
		std::string a = singleArchiveSelection();
		if (a.empty()) { NextZipDialogs::alert("Test archive", "Select a single archive file first."); return; }
		NextZipEngine eng;
		if (!eng.open(a)) { NextZipDialogs::alert("Test failed", eng.error()); return; }
		std::vector<uint32_t> none;
		bool ok = testWithEngine(&eng, none, "", sBaseName(a), nullptr);
		unsigned long long files = 0, folders = 0, size = 0, packed = 0;
		for (const NZEntry& e : eng.entries()) { if (e.isDir) folders++; else { files++; size += e.size; packed += e.packSize; } }
		char msg[1024];
		snprintf(msg, sizeof(msg),
			"Archive: %s\n\nArchives: 1\nPacked Size: %llu bytes\nFolders: %llu\nFiles: %llu\nSize: %llu bytes\n\n%s",
			sBaseName(a).c_str(), packed, folders, files, size,
			ok ? "There are no errors" : eng.error().c_str());
		NextZipDialogs::showInfoTitle("Testing", msg);
	}

	void fsDelete() {
		std::vector<std::string> paths = selectedFsPaths();
		if (paths.empty()) { NextZipDialogs::alert("Delete", "Select files or folders first."); return; }
		char title[128];
		snprintf(title, sizeof(title), "Move %zu item%s to the Trash?", paths.size(), paths.size() == 1 ? "" : "s");
		std::string info = paths.size() == 1 ? paths[0] : "The selected items will be moved to the Trash.";
		if (!NextZipDialogs::confirm(title, info, "Move to Trash", "Cancel")) return;
		for (const std::string& p : paths) {
			GFile* f = g_file_new_for_path(p.c_str());
			g_file_trash(f, nullptr, nullptr);
			g_object_unref(f);
		}
		refreshFs();
	}

	void fsInfo() { computeChecksumForSelection("SHA256"); }
	void computeChecksumForSelection(const std::string& algo) {
		std::vector<std::string> paths = selectedFsPaths();
		if (paths.empty()) { NextZipDialogs::alert("Checksum", "Select a file or folder first."); return; }
		unsigned long long folders = 0, files = 0, size = 0;
		std::string singleFile = (paths.size() == 1 && !pathIsDir(paths[0])) ? paths[0] : "";
		std::function<void(const std::string&)> walk = [&](const std::string& p) {
			if (!pathIsDir(p)) {
				files++;
				GStatBuf st;
				if (g_stat(p.c_str(), &st) == 0) size += (unsigned long long)st.st_size;
				return;
			}
			folders++;
			GDir* gd = g_dir_open(p.c_str(), 0, nullptr);
			if (!gd) return;
			const gchar* f;
			while ((f = g_dir_read_name(gd)) != nullptr) walk(p + "/" + f);
			g_dir_close(gd);
		};
		for (const std::string& p : paths) {
			if (pathIsDir(p)) walk(p);
			else {
				files++;
				GStatBuf st;
				if (g_stat(p.c_str(), &st) == 0) size += (unsigned long long)st.st_size;
			}
		}
		char head[256];
		snprintf(head, sizeof(head), "Folders: %llu\nFiles:   %llu\nSize:    %llu bytes\n", folders, files, size);
		std::string msg = head;
		if (!singleFile.empty()) {
			msg += "\nFile: " + sBaseName(singleFile) + "\n";
			std::vector<std::string> algos{"CRC32"};
			if (algo != "CRC32") algos.push_back(algo);
			for (const std::string& a : algos) {
				std::string hex, err;
				char line[512];
				if (NextZipEngine::checksumFile(singleFile, a, hex, err))
					snprintf(line, sizeof(line), "%-7s %s\n", a.c_str(), hex.c_str());
				else
					snprintf(line, sizeof(line), "%-7s (%s)\n", a.c_str(), err.c_str());
				msg += line;
			}
		} else {
			msg += "\n(Select a single file to compute its checksum.)";
		}
		NextZipDialogs::showInfoTitle("Checksum information", msg);
	}

	// ═══════════════════════════════════════════════════════════════════════
	// BOTTOM-PANE extras: open-in-editor / delete-from-archive
	// ═══════════════════════════════════════════════════════════════════════
	void arcOpenInEditorMenu() {
		FMNode* n = selectedArcNode();
		if (n && !n->isDir && n->entryIndex >= 0) openEntryInEditor(n);
	}
	void arcDelete() {
		if (!root) return;
		std::vector<uint32_t> idx = selectedIndices();
		if (idx.empty()) { NextZipDialogs::alert("Delete", "Select entries to delete."); return; }
		char title[128];
		snprintf(title, sizeof(title), "Delete %zu item%s from the archive?", idx.size(), idx.size() == 1 ? "" : "s");
		if (!NextZipDialogs::confirm(title, "This rewrites the archive without the selected entries.",
		                             "Delete", "Cancel"))
			return;
		if (!engine->deleteEntries(idx)) {
			NextZipDialogs::alert("Delete failed", engine->error());
			return;
		}
		// nested (.tar.gz): the inner temp changed → re-wrap outward to the real file.
		if (layers.size() > 1) {
			std::string err;
			if (!rewrapOutwardFromInner(err))
				NextZipDialogs::alert("Saved inside, but couldn't update the outer archive", err);
		}
		freeTree(root); root = buildTree(engine->entries());
		navigateTo(root);
	}

	// ═══════════════════════════════════════════════════════════════════════
	// Right-click context menus
	// ═══════════════════════════════════════════════════════════════════════
	void fsSelectRowAt(GtkWidget* view, double x, double y) {
		GtkTreePath* path = nullptr;
		if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (int)x, (int)y, &path, nullptr, nullptr, nullptr)) {
			GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
			if (!gtk_tree_selection_path_is_selected(sel, path)) {
				gtk_tree_selection_unselect_all(sel);
				gtk_tree_selection_select_path(sel, path);
			}
			gtk_tree_path_free(path);
		}
	}
	void fsContextMenu(double x, double y) {
		fsSelectRowAt(fsView, x, y);
		std::vector<std::string> sel = selectedFsPaths();
		if (sel.empty()) return;
		bool single = (sel.size() == 1);
		const std::string& first = sel.front();
		bool singleArchive = single && looksLikeArchive(first);
		std::string nameNoExt = sStripExt(sBaseName(first));

		PopupMenu pm;
		if (singleArchive) {
			pm.add("Extract files…",  [this]{ fsExtract(); });
			pm.add("Extract Here",    [this]{ fsExtractHere(); });
			pm.add("Extract to \"" + nameNoExt + "/\"", [this]{ fsExtractToSub(); });
			pm.add("Test archive",    [this]{ fsTest(); });
			pm.separator();
		}
		std::string qbase = single ? nameNoExt : sBaseName(sDirName(first));
		if (qbase.empty()) qbase = "Archive";
		pm.add("Add to archive…", [this]{ fsAdd(); });
		pm.add("Add to \"" + qbase + ".7z\"",  [this]{ fsAddQuick("7z"); });
		pm.add("Add to \"" + qbase + ".zip\"", [this]{ fsAddQuick("zip"); });
		pm.add("Add to \"" + qbase + ".tgz\"", [this]{ fsAddQuick("tgz"); });
		pm.separator();
		pm.submenu("CRC SHA", {"CRC32","MD5","SHA1","SHA256","SHA384","SHA512"},
		           [this](const std::string& a) { computeChecksumForSelection(a); });
		pm.popupAt(fsView, x, y);
	}
	void arcContextMenu(double x, double y) {
		if (!root) return;
		fsSelectRowAt(arcView, x, y);
		FMNode* n = selectedArcNode();
		PopupMenu pm;
		if (n && !n->isDir && n->entryIndex >= 0)
			pm.add("Open in Editor", [this]{ arcOpenInEditorMenu(); });
		pm.add("Extract…", [this]{ actExtract(); });
		pm.add("Test",     [this]{ actTest(); });
		pm.add("Info",     [this]{ actInfo(); });
		pm.separator();
		pm.add("Delete from archive", [this]{ arcDelete(); });
		pm.popupAt(arcView, x, y);
	}

	// ── bottom toolbar: extract / test / info ────────────────────────────────
	// Reload the on-screen archive from disk (it may have changed underneath us),
	// staying in the current folder when it still exists after the reload.
	void actRefresh() {
		if (displayPath.empty()) return;
		std::vector<std::string> cwdPath = cwd ? nodePath(cwd) : std::vector<std::string>();
		std::string path = displayPath;               // openArchive re-stamps displayPath
		openArchive(path, false);
		if (!root || cwdPath.empty()) return;
		FMNode* dest = findDirByPath(root, cwdPath);
		if (dest) navigateTo(dest);
	}

	void actExtract() {
		if (!root) return;
		std::vector<uint32_t> idx = selectedIndices();   // empty = extract everything
		// macOS uses a folder chooser with prompt "Extract Here" — same here.
		std::string dest = NextZipDialogs::chooseExtractFolder("");
		if (dest.empty()) return;
		std::string usedPw;
		if (extractWithEngine(engine.get(), idx, dest, havePassword ? archivePassword : "",
		                      false, 0, false, sBaseName(archivePath), &usedPw)) {
			archivePassword = usedPw; havePassword = !usedPw.empty();
			NextZipDialogs::alert("Extraction complete", "Extracted to:\n" + dest);
		}
	}
	void actTest() {
		if (!root) return;
		std::vector<uint32_t> idx = selectedIndices();
		std::string usedPw;
		if (testWithEngine(engine.get(), idx, havePassword ? archivePassword : "",
		                   sBaseName(archivePath), &usedPw)) {
			archivePassword = usedPw; havePassword = !usedPw.empty();
			NextZipDialogs::alert("Test passed", "No errors detected (CRCs OK).");
		} else if (engine->lastErrorNeedsPassword()) {
			return;   // user cancelled the password prompt — no nag
		} else {
			NextZipDialogs::alert("Test failed", engine->error());
		}
	}
	void actInfo() {
		FMNode* n = selectedArcNode();
		if (!n) {
			std::string nested;
			if (layers.size() > 1) {
				char b[64]; snprintf(b, sizeof(b), "\n(unwrapped %zu nested layers)", layers.size() - 1);
				nested = b;
			}
			char msg[1024];
			snprintf(msg, sizeof(msg), "Archive: %s\nFormat: %s\nItems: %zu%s",
				(!displayPath.empty() ? displayPath : archivePath).c_str(),
				engine->format().c_str(), engine->entries().size(), nested.c_str());
			NextZipDialogs::alert("NextZip", msg);
			return;
		}
		if (n->entryIndex < 0) { NextZipDialogs::alert("Folder", n->name); return; }
		const NZEntry& e = engine->entries()[n->entryIndex];
		char crc[16] = "—";
		if (e.hasCrc) snprintf(crc, sizeof(crc), "%08X", e.crc);
		char msg[1024];
		snprintf(msg, sizeof(msg), "Size: %llu\nPacked: %llu\nCRC: %s\nMethod: %s\nEncrypted: %s",
			(unsigned long long)e.size, (unsigned long long)e.packSize,
			crc, e.method.c_str(), e.encrypted ? "yes" : "no");
		NextZipDialogs::alert(e.path, msg);
	}
};

G_GNUC_END_IGNORE_DEPRECATIONS

// ─────────────────────────────────────────────────────────────────────────────
NextZipController::NextZipController() {
	d = new Impl();
	d->self = this;
	d->host = &m_host;
	d->engine.reset(new NextZipEngine());
}
NextZipController::~NextZipController() {
	freeTree(d->root);
	delete d;
}

GtkWidget* NextZipController::panelView() { d->ensurePanel(); return d->panel; }
std::string NextZipController::currentArchivePath() const { return d->displayPath; }

void NextZipController::showOpenPanel() { d->showOpenPanel(); }
void NextZipController::openArchiveAtPath(const std::string& path, bool quiet) { d->openArchive(path, quiet); }
void NextZipController::openCurrentEditorFile() {
	std::string p = m_host ? m_host->currentFilePath() : "";
	if (!p.empty()) openArchiveAtPath(p);
	else            showOpenPanel();
}
void NextZipController::handleFileSaved(const std::string& path) { d->handleFileSaved(path); }
void NextZipController::showAbout() {
	NextZipDialogs::alert("NextZip",
		"Archive manager for Nextpad++.\n\nEngine: 7-Zip (LGPL).\n"
		"Extracts every format 7-Zip supports, including RAR / RAR5.\n"
		"RAR is extraction-only (unRAR license — RAR archives cannot be created).\n\nGPL.");
}
