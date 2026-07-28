/*
 * NextZipDialogs.cpp — programmatic modal GTK4 dialogs (Add / Extract / Info /
 * Password), faithful to the Windows 7-Zip windows via the macOS port: control
 * layout, per-format enable/disable, method/dict/word lists and the property
 * mapping mirror CompressDialog.cpp / UpdateGUI.cpp.
 *
 * Modality: every dialog runs a nested GMainLoop (GTK4 has no gtk_dialog_run) —
 * the established pattern across the Nextpad++ Linux plugin ports. Dialog
 * buttons close via gtk_window_close (NEVER gtk_window_destroy: destroy does
 * not emit close-request, which is what quits the loop).
 *
 * NextZip 2026 (GPL).
 */
#include "NextZipDialogs.h"

#include <cstring>
#include <cstdio>
#include <functional>
#include <thread>

namespace {

GtkWindow* g_parent = nullptr;

// ── modal plumbing ───────────────────────────────────────────────────────────
struct Dlg {
	GtkWidget* win = nullptr;
	GMainLoop* loop = nullptr;
	bool       ok = false;
};
void dlgClose(Dlg* d, bool ok) { d->ok = ok; gtk_window_close(GTK_WINDOW(d->win)); }

GtkWidget* dlgNew(Dlg* d, const char* title, bool resizable = false) {
	d->win = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(d->win), title);
	gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
	if (g_parent) gtk_window_set_transient_for(GTK_WINDOW(d->win), g_parent);
	gtk_window_set_resizable(GTK_WINDOW(d->win), resizable);
	// CRITICAL: without this, gtk_window_close() DESTROYS the window and its
	// whole child tree, and every widget read after dlgRun() is a
	// use-after-free (seen live: GTK_IS_EDITABLE assertion → NULL →
	// std::string(NULL) → std::terminate). Close now only hides; DlgGuard
	// destroys the window after the values have been read.
	gtk_window_set_hide_on_close(GTK_WINDOW(d->win), TRUE);

	GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(outer, 16); gtk_widget_set_margin_end(outer, 16);
	gtk_widget_set_margin_top(outer, 16);   gtk_widget_set_margin_bottom(outer, 16);
	gtk_window_set_child(GTK_WINDOW(d->win), outer);

	// One close-request handler for the dialog's LIFETIME (dlgRun may be called
	// again after a validation failure re-presents the window).
	g_signal_connect(d->win, "close-request",
		G_CALLBACK(+[](GtkWindow*, gpointer u) -> gboolean {
			Dlg* d = (Dlg*)u;
			if (d->loop && g_main_loop_is_running(d->loop)) g_main_loop_quit(d->loop);
			return FALSE;
		}), d);

	// Escape cancels (macOS: Cancel carries the \033 key equivalent).
	GtkEventController* keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed",
		G_CALLBACK(+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer u) -> gboolean {
			if (keyval == GDK_KEY_Escape) { dlgClose((Dlg*)u, false); return TRUE; }
			return FALSE;
		}), d);
	gtk_widget_add_controller(d->win, keys);
	return outer;
}

// Destroys the dialog window at scope exit — AFTER all widget reads.
struct DlgGuard {
	Dlg* d;
	explicit DlgGuard(Dlg* dd) : d(dd) {}
	~DlgGuard() { if (d && d->win) { gtk_window_destroy(GTK_WINDOW(d->win)); d->win = nullptr; } }
};

void dlgRun(Dlg* d) {
	d->loop = g_main_loop_new(nullptr, FALSE);
	gtk_window_present(GTK_WINDOW(d->win));
	g_main_loop_run(d->loop);
	g_main_loop_unref(d->loop);
	d->loop = nullptr;
}

// OK/Cancel row; wires both to dlgClose. Returns the OK button.
GtkWidget* dlgButtons(GtkWidget* outer, Dlg* d,
                      const char* okLabel = "OK", const char* cancelLabel = "Cancel") {
	GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(row, GTK_ALIGN_END);
	GtkWidget* cancel = gtk_button_new_with_label(cancelLabel);
	GtkWidget* ok = gtk_button_new_with_label(okLabel);
	gtk_widget_add_css_class(ok, "suggested-action");
	g_object_set_data(G_OBJECT(cancel), "dlg", d);
	g_object_set_data(G_OBJECT(ok), "dlg", d);
	g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
		dlgClose((Dlg*)g_object_get_data(G_OBJECT(b), "dlg"), false);
	}), nullptr);
	g_signal_connect(ok, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
		dlgClose((Dlg*)g_object_get_data(G_OBJECT(b), "dlg"), true);
	}), nullptr);
	gtk_box_append(GTK_BOX(row), cancel);
	gtk_box_append(GTK_BOX(row), ok);
	gtk_box_append(GTK_BOX(outer), row);
	return ok;
}

GtkWidget* label(const char* s) {
	GtkWidget* l = gtk_label_new(s);
	gtk_label_set_xalign(GTK_LABEL(l), 1.0f);   // grid labels right-align (NSGridCellPlacementTrailing)
	return l;
}
GtkWidget* leftLabel(const char* s) {
	GtkWidget* l = gtk_label_new(s);
	gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
	return l;
}

// ── value-carrying dropdown (replaces NSPopUpButton + representedObject) ─────
// Items are (title, value) pairs; value "" = auto/none.
struct Popup {
	GtkWidget*               dd = nullptr;    // GtkDropDown
	std::vector<std::string> values;

	void fill(const std::vector<std::pair<std::string, std::string>>& items) {
		values.clear();
		GtkStringList* sl = gtk_string_list_new(nullptr);
		for (auto& it : items) { gtk_string_list_append(sl, it.first.c_str()); values.push_back(it.second); }
		gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(sl));
		g_object_unref(sl);
		if (!values.empty()) gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 0);
	}
	std::string value() const {
		guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
		return (i != GTK_INVALID_LIST_POSITION && i < values.size()) ? values[i] : std::string();
	}
	std::string title() const {
		guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
		if (i == GTK_INVALID_LIST_POSITION) return {};
		GListModel* m = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
		const char* s = gtk_string_list_get_string(GTK_STRING_LIST(m), i);
		return s ? s : "";
	}
	int index() const {
		guint i = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
		return i == GTK_INVALID_LIST_POSITION ? 0 : (int)i;
	}
	void selectTitle(const std::string& t) {
		GListModel* m = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
		if (!m) return;
		guint n = g_list_model_get_n_items(m);
		for (guint i = 0; i < n; i++) {
			const char* s = gtk_string_list_get_string(GTK_STRING_LIST(m), i);
			if (s && t == s) { gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i); return; }
		}
	}
	uint64_t valueU64() const { const std::string v = value(); return v.empty() ? 0 : strtoull(v.c_str(), nullptr, 10); }
	int      valueInt() const { const std::string v = value(); return v.empty() ? 0 : atoi(v.c_str()); }
	bool     enabled() const { return gtk_widget_get_sensitive(dd); }
	void     setEnabled(bool on) { gtk_widget_set_sensitive(dd, on); }
};
Popup makePopup() {
	Popup p;
	p.dd = gtk_drop_down_new(nullptr, nullptr);
	gtk_widget_set_size_request(p.dd, 150, -1);
	return p;
}

std::string extForFormat(const std::string& f) {
	if (f == "zip")   return "zip";
	if (f == "tar")   return "tar";
	if (f == "gzip")  return "gz";
	if (f == "bzip2") return "bz2";
	if (f == "xz")    return "xz";
	return "7z";
}

std::string trimWs(const std::string& s) {
	size_t b = s.find_first_not_of(" \t");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t");
	return s.substr(b, e - b + 1);
}

std::string dirName(const std::string& p) {
	size_t sl = p.find_last_of('/');
	return sl == std::string::npos ? "" : p.substr(0, sl);
}
std::string baseName(const std::string& p) {
	size_t sl = p.find_last_of('/');
	return sl == std::string::npos ? p : p.substr(sl + 1);
}
std::string stripExt(const std::string& p) {
	size_t dot = p.find_last_of('.');
	return (dot == std::string::npos || dot == 0) ? p : p.substr(0, dot);
}

// Sync GtkFileDialog wrappers (async API pumped by a nested loop).
struct FileDlgCtx { std::string path; bool ok = false; GMainLoop* loop = nullptr; };

std::string chooseFolder(const std::string& startDir) {
	GtkFileDialog* fd = gtk_file_dialog_new();
	if (!startDir.empty()) {
		GFile* f = g_file_new_for_path(startDir.c_str());
		gtk_file_dialog_set_initial_folder(fd, f);
		g_object_unref(f);
	}
	FileDlgCtx ctx; ctx.loop = g_main_loop_new(nullptr, FALSE);
	gtk_file_dialog_select_folder(fd, g_parent, nullptr,
		+[](GObject* src, GAsyncResult* res, gpointer u) {
			FileDlgCtx* c = (FileDlgCtx*)u;
			GFile* f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
			if (f) { gchar* p = g_file_get_path(f); if (p) { c->path = p; c->ok = true; g_free(p); } g_object_unref(f); }
			if (g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
		}, &ctx);
	g_main_loop_run(ctx.loop);
	g_main_loop_unref(ctx.loop);
	g_object_unref(fd);
	return ctx.ok ? ctx.path : std::string();
}

std::string chooseSavePath(const std::string& startDir, const std::string& name) {
	GtkFileDialog* fd = gtk_file_dialog_new();
	if (!startDir.empty()) {
		GFile* f = g_file_new_for_path(startDir.c_str());
		gtk_file_dialog_set_initial_folder(fd, f);
		g_object_unref(f);
	}
	if (!name.empty()) gtk_file_dialog_set_initial_name(fd, name.c_str());
	FileDlgCtx ctx; ctx.loop = g_main_loop_new(nullptr, FALSE);
	gtk_file_dialog_save(fd, g_parent, nullptr,
		+[](GObject* src, GAsyncResult* res, gpointer u) {
			FileDlgCtx* c = (FileDlgCtx*)u;
			GFile* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr);
			if (f) { gchar* p = g_file_get_path(f); if (p) { c->path = p; c->ok = true; g_free(p); } g_object_unref(f); }
			if (g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
		}, &ctx);
	g_main_loop_run(ctx.loop);
	g_main_loop_unref(ctx.loop);
	g_object_unref(fd);
	return ctx.ok ? ctx.path : std::string();
}

} // namespace

namespace NextZipDialogs {

void setParentWindow(GtkWindow* parent) { g_parent = parent; }

std::string chooseOpenFile(const std::string& startDir);   // fwd (defined below, used by controller too)

void alert(const std::string& title, const std::string& info) {
	GtkAlertDialog* a = gtk_alert_dialog_new("%s", title.c_str());
	if (!info.empty()) gtk_alert_dialog_set_detail(a, info.c_str());
	gtk_alert_dialog_show(a, g_parent);
	g_object_unref(a);
}

bool confirm(const std::string& title, const std::string& info,
             const std::string& okLabel, const std::string& cancelLabel) {
	GtkAlertDialog* a = gtk_alert_dialog_new("%s", title.c_str());
	if (!info.empty()) gtk_alert_dialog_set_detail(a, info.c_str());
	const char* buttons[] = { okLabel.c_str(), cancelLabel.c_str(), nullptr };
	gtk_alert_dialog_set_buttons(a, buttons);
	gtk_alert_dialog_set_default_button(a, 0);
	gtk_alert_dialog_set_cancel_button(a, 1);
	struct Ctx { int result = 1; GMainLoop* loop; } ctx { 1, g_main_loop_new(nullptr, FALSE) };
	gtk_alert_dialog_choose(a, g_parent, nullptr,
		+[](GObject* src, GAsyncResult* res, gpointer u) {
			Ctx* c = (Ctx*)u;
			c->result = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
			if (g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
		}, &ctx);
	g_main_loop_run(ctx.loop);
	g_main_loop_unref(ctx.loop);
	g_object_unref(a);
	return ctx.result == 0;
}

// ════════════════════════════════════════════════════════════════════════════
// Add to Archive  (full Windows-faithful dialog)
// ════════════════════════════════════════════════════════════════════════════
namespace {
struct AddState {
	Dlg        d;
	GtkWidget* pathField;
	Popup      fmt, level, method, dict, word, solid, threads, mem, update, pathModeP, encMethod;
	GtkWidget* threadsMaxLabel;
	GtkWidget* memCompLabel;
	GtkWidget* memDecompLabel;
	GtkWidget* splitField;
	GtkWidget* paramsField;
	GtkWidget* sfxCheck;
	GtkWidget* sharedCheck;
	GtkWidget* deleteCheck;
	GtkWidget* pwField;     // GtkPasswordEntry (its own peek icon replaces "Show Password")
	GtkWidget* pwField2;
	GtkWidget* encNamesCheck;
	bool       building = false;   // suppress change-callbacks during rebuild
};

void addUpdateMem(AddState* st) {
	uint64_t dict = st->dict.valueU64();
	if (dict == 0) {
		gtk_label_set_text(GTK_LABEL(st->memCompLabel), "auto");
		gtk_label_set_text(GTK_LABEL(st->memDecompLabel), "auto");
		return;
	}
	const uint64_t M = 1024 * 1024;
	std::string m = st->method.title();
	char buf[64];
	if (m == "LZMA" || m == "LZMA2") {
		snprintf(buf, sizeof(buf), "~%llu MB", (unsigned long long)(dict / M * 11 + 64));
		gtk_label_set_text(GTK_LABEL(st->memCompLabel), buf);
		snprintf(buf, sizeof(buf), "~%llu MB", (unsigned long long)(dict / M + 2));
		gtk_label_set_text(GTK_LABEL(st->memDecompLabel), buf);
	} else {
		snprintf(buf, sizeof(buf), "~%llu MB", (unsigned long long)(dict / M + 16));
		gtk_label_set_text(GTK_LABEL(st->memCompLabel), buf);
		snprintf(buf, sizeof(buf), "~%llu MB", (unsigned long long)(dict / M + 2));
		gtk_label_set_text(GTK_LABEL(st->memDecompLabel), buf);
	}
}

void addMethodChanged(AddState* st) {
	std::string m = st->method.title();
	bool store = (st->level.valueInt() == 0 && st->level.value() != "");
	// level.value "" is never used for levels (all carry numbers); store = level 0
	store = (st->level.valueInt() == 0);
	bool isLZMA = (m == "LZMA" || m == "LZMA2");
	bool isPPMd = (m == "PPMd");
	bool isBZip = (m == "BZip2");
	bool isDeflate = (m == "Deflate" || m == "Deflate64");

	// dictionary list
	std::vector<std::pair<std::string, std::string>> dicts{{"* auto", ""}};
	char t[64], v[64];
	if (isLZMA) {
		const uint64_t K = 1024, M = 1024 * 1024;
		const uint64_t sizes[] = {64*K,128*K,256*K,512*K,1*M,2*M,3*M,4*M,6*M,8*M,12*M,16*M,24*M,32*M,48*M,64*M,96*M,128*M,192*M,256*M,384*M,512*M,768*M,1024*M,1536*M};
		for (uint64_t s : sizes) {
			if (s >= M) snprintf(t, sizeof(t), "%llu MB", (unsigned long long)(s / M));
			else        snprintf(t, sizeof(t), "%llu KB", (unsigned long long)(s / K));
			snprintf(v, sizeof(v), "%llu", (unsigned long long)s);
			dicts.push_back({t, v});
		}
	} else if (isPPMd) {
		const uint64_t M = 1024 * 1024;
		const uint64_t mbs[] = {1,2,4,8,16,32,64,128,256,512,1024};
		for (uint64_t mb : mbs) {
			snprintf(t, sizeof(t), "%llu MB", (unsigned long long)mb);
			snprintf(v, sizeof(v), "%llu", (unsigned long long)(mb * M));
			dicts.push_back({t, v});
		}
	} else if (isBZip) {
		const uint64_t K = 1024;
		const uint64_t kbs[] = {100,200,300,400,500,600,700,800,900};
		for (uint64_t kb : kbs) {
			snprintf(t, sizeof(t), "%llu KB", (unsigned long long)kb);
			snprintf(v, sizeof(v), "%llu", (unsigned long long)(kb * K));
			dicts.push_back({t, v});
		}
	}
	st->dict.fill(dicts);
	st->dict.setEnabled(!store && (isLZMA || isPPMd || isBZip));

	// word size / order list
	std::vector<std::pair<std::string, std::string>> words{{"* auto", ""}};
	const int lzmaW[] = {8,12,16,24,32,48,64,96,128,192,273};
	const int ppmdO[] = {2,3,4,5,6,8,10,12,16,24,32};
	const int deflW[] = {8,16,32,64,128,258};
	auto pushInts = [&](const int* a, size_t n) {
		for (size_t i = 0; i < n; i++) { snprintf(t, sizeof(t), "%d", a[i]); words.push_back({t, t}); }
	};
	if (isLZMA)         pushInts(lzmaW, sizeof(lzmaW)/sizeof(int));
	else if (isPPMd)    pushInts(ppmdO, sizeof(ppmdO)/sizeof(int));
	else if (isDeflate) pushInts(deflW, sizeof(deflW)/sizeof(int));
	st->word.fill(words);
	st->word.setEnabled(!store && (isLZMA || isPPMd || isDeflate));

	addUpdateMem(st);
}

void addFormatChanged(AddState* st) {
	std::string f = st->fmt.title();
	std::string p = gtk_editable_get_text(GTK_EDITABLE(st->pathField));
	if (!p.empty())
		gtk_editable_set_text(GTK_EDITABLE(st->pathField), (stripExt(p) + "." + extForFormat(f)).c_str());

	bool is7z = (f == "7z"), isZip = (f == "zip"), isTar = (f == "tar");
	bool solidOK = is7z || f == "xz";
	bool mtOK    = is7z || isZip || f == "bzip2" || f == "xz";
	bool encOK   = is7z || isZip;
	bool memOK   = !isTar;

	// methods
	std::vector<std::pair<std::string, std::string>> methods;
	auto M = [&](const char* n) { methods.push_back({n, n}); };
	if (is7z)             { M("LZMA2"); M("LZMA"); M("PPMd"); M("BZip2"); M("Deflate"); M("Deflate64"); M("Copy"); }
	else if (isZip)       { M("Deflate"); M("Deflate64"); M("BZip2"); M("LZMA"); M("PPMd"); }
	else if (f == "gzip") { M("Deflate"); }
	else if (f == "bzip2"){ M("BZip2"); }
	else if (f == "xz")   { M("LZMA2"); }
	st->method.fill(methods);
	st->method.setEnabled(!isTar && !methods.empty());

	// levels (mask per format)
	const struct { const char* title; int n; } allLevels[] = {
		{"0 — Store",0},{"1 — Fastest",1},{"3 — Fast",3},{"5 — Normal",5},{"7 — Maximum",7},{"9 — Ultra",9}};
	std::vector<std::pair<std::string, std::string>> levs;
	for (auto& lv : allLevels) {
		int n = lv.n; bool ok;
		if (isTar)             ok = (n == 0);
		else if (f == "gzip")  ok = (n==1||n==5||n==7||n==9);
		else if (f == "bzip2") ok = (n==1||n==3||n==5||n==7||n==9);
		else if (f == "xz")    ok = (n != 0);
		else                   ok = (n==0||n==1||n==3||n==5||n==7||n==9);   // 7z, zip
		if (ok) { char v[8]; snprintf(v, sizeof(v), "%d", n); levs.push_back({lv.title, v}); }
	}
	st->level.fill(levs);
	st->level.selectTitle("5 — Normal");
	st->level.setEnabled(!isTar);

	st->solid.setEnabled(solidOK);
	st->threads.setEnabled(mtOK);
	st->mem.setEnabled(memOK);

	// encryption method list
	std::vector<std::pair<std::string, std::string>> encs;
	if (is7z)       encs.push_back({"AES-256", "AES256"});
	else if (isZip) { encs.push_back({"ZipCrypto", "ZipCrypto"}); encs.push_back({"AES-256", "AES256"}); }
	st->encMethod.fill(encs);
	gtk_widget_set_sensitive(st->pwField, encOK);
	gtk_widget_set_sensitive(st->pwField2, encOK);
	st->encMethod.setEnabled(encOK);
	gtk_widget_set_sensitive(st->encNamesCheck, is7z);
	if (!is7z) gtk_check_button_set_active(GTK_CHECK_BUTTON(st->encNamesCheck), FALSE);
	gtk_widget_set_sensitive(st->sfxCheck, FALSE);   // SFX module not shipped on Linux
	gtk_widget_set_tooltip_text(st->sfxCheck, "SFX archives are not supported in the Linux build");

	addMethodChanged(st);
}
} // namespace

NZAddOptions runAddForInputs(const std::vector<std::string>& inputs) {
	NZAddOptions o;
	if (inputs.empty()) return o;
	const std::string& first = inputs.front();
	std::string dir = dirName(first);
	std::string base = inputs.size() == 1 ? stripExt(baseName(first)) : baseName(dir);
	if (base.empty()) base = "Archive";
	std::string def = dir + "/" + base + ".7z";

	AddState st;
	GtkWidget* outer = dlgNew(&st.d, "Add to Archive");
	DlgGuard guard(&st.d);

	// path row
	GtkWidget* pathRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(pathRow), leftLabel("Archive:"));
	st.pathField = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(st.pathField), def.c_str());
	gtk_widget_set_size_request(st.pathField, 480, -1);
	gtk_widget_set_hexpand(st.pathField, TRUE);
	gtk_box_append(GTK_BOX(pathRow), st.pathField);
	GtkWidget* dots = gtk_button_new_with_label("…");
	g_object_set_data(G_OBJECT(dots), "st", &st);
	g_signal_connect(dots, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
		AddState* st = (AddState*)g_object_get_data(G_OBJECT(b), "st");
		std::string cur = gtk_editable_get_text(GTK_EDITABLE(st->pathField));
		std::string got = chooseSavePath(dirName(cur), baseName(cur));
		if (!got.empty()) gtk_editable_set_text(GTK_EDITABLE(st->pathField), got.c_str());
	}), nullptr);
	gtk_box_append(GTK_BOX(pathRow), dots);
	gtk_box_append(GTK_BOX(outer), pathRow);

	// two columns
	GtkWidget* columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
	gtk_box_append(GTK_BOX(outer), columns);

	// left grid
	GtkWidget* leftGrid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(leftGrid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(leftGrid), 8);
	gtk_widget_set_valign(leftGrid, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(columns), leftGrid);

	st.fmt = makePopup(); st.level = makePopup(); st.method = makePopup();
	st.dict = makePopup(); st.word = makePopup(); st.solid = makePopup();
	st.threads = makePopup(); st.mem = makePopup();
	st.fmt.fill({{"7z","7z"},{"zip","zip"},{"tar","tar"},{"gzip","gzip"},{"bzip2","bzip2"},{"xz","xz"}});
	st.solid.fill({{"* auto",""},{"Non-solid","off"},{"1 MB","1048576b"},{"4 MB","4194304b"},
		{"16 MB","16777216b"},{"64 MB","67108864b"},{"256 MB","268435456b"},{"1 GB","1073741824b"},
		{"4 GB","4294967296b"},{"8 GB","8589934592b"},{"Solid","on"}});
	unsigned maxThreads = std::max(1u, std::thread::hardware_concurrency());
	{
		std::vector<std::pair<std::string, std::string>> th{{"* auto", ""}};
		for (unsigned i = 1; i <= maxThreads; i++) { char b[8]; snprintf(b, sizeof(b), "%u", i); th.push_back({b, b}); }
		st.threads.fill(th);
	}
	{
		std::vector<std::pair<std::string, std::string>> mu{{"* auto", ""}};
		const int pcts[] = {10,20,30,40,50,60,70,80,90,100};
		for (int p : pcts) { char b[8]; snprintf(b, sizeof(b), "%d%%", p); mu.push_back({b, b}); }
		st.mem.fill(mu);
	}
	char tmBuf[16]; snprintf(tmBuf, sizeof(tmBuf), "/ %u", maxThreads);
	st.threadsMaxLabel = leftLabel(tmBuf);
	st.memCompLabel = leftLabel("auto");
	st.memDecompLabel = leftLabel("auto");
	st.splitField = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(st.splitField), "e.g. 100m (optional)");
	st.paramsField = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(st.paramsField), "advanced, e.g. tc=off");

	int lrow = 0;
	auto addLeft = [&](const char* lab, GtkWidget* w) {
		gtk_grid_attach(GTK_GRID(leftGrid), label(lab), 0, lrow, 1, 1);
		gtk_grid_attach(GTK_GRID(leftGrid), w, 1, lrow, 1, 1);
		lrow++;
	};
	addLeft("Archive format:",     st.fmt.dd);
	addLeft("Compression level:",  st.level.dd);
	addLeft("Compression method:", st.method.dd);
	addLeft("Dictionary size:",    st.dict.dd);
	addLeft("Word size:",          st.word.dd);
	addLeft("Solid Block size:",   st.solid.dd);
	{
		GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_box_append(GTK_BOX(r), st.threads.dd);
		gtk_box_append(GTK_BOX(r), st.threadsMaxLabel);
		addLeft("Number of CPU threads:", r);
	}
	{
		GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_box_append(GTK_BOX(r), st.memCompLabel);
		gtk_box_append(GTK_BOX(r), st.mem.dd);
		addLeft("Memory for Compressing:", r);
	}
	addLeft("Memory for Decompressing:", st.memDecompLabel);
	addLeft("Split to volumes, bytes:",  st.splitField);
	addLeft("Parameters:",               st.paramsField);

	// right column
	GtkWidget* rightCol = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_valign(rightCol, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(columns), rightCol);

	GtkWidget* rtGrid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(rtGrid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(rtGrid), 8);
	st.update = makePopup();
	st.update.fill({{"Add and replace files","0"},{"Update and add files","1"},
	                {"Freshen existing files","2"},{"Synchronize files","3"}});
	st.pathModeP = makePopup();
	st.pathModeP.fill({{"Relative pathnames","0"},{"Full pathnames","1"},{"Absolute pathnames","2"}});
	gtk_grid_attach(GTK_GRID(rtGrid), label("Update mode:"), 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(rtGrid), st.update.dd,          1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(rtGrid), label("Path mode:"),   0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(rtGrid), st.pathModeP.dd,       1, 1, 1, 1);
	gtk_box_append(GTK_BOX(rightCol), rtGrid);

	// Options box
	GtkWidget* optsFrame = gtk_frame_new("Options");
	GtkWidget* optsBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(optsBox, 8); gtk_widget_set_margin_end(optsBox, 8);
	gtk_widget_set_margin_top(optsBox, 6);   gtk_widget_set_margin_bottom(optsBox, 6);
	st.sfxCheck    = gtk_check_button_new_with_label("Create SFX archive");
	st.sharedCheck = gtk_check_button_new_with_label("Compress shared files");
	st.deleteCheck = gtk_check_button_new_with_label("Delete files after compression");
	gtk_box_append(GTK_BOX(optsBox), st.sfxCheck);
	gtk_box_append(GTK_BOX(optsBox), st.sharedCheck);
	gtk_box_append(GTK_BOX(optsBox), st.deleteCheck);
	gtk_frame_set_child(GTK_FRAME(optsFrame), optsBox);
	gtk_box_append(GTK_BOX(rightCol), optsFrame);

	// Encryption box (GtkPasswordEntry has a built-in reveal toggle, replacing
	// the macOS "Show Password" checkbox + parallel plain fields)
	GtkWidget* encFrame = gtk_frame_new("Encryption");
	GtkWidget* encGrid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(encGrid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(encGrid), 8);
	gtk_widget_set_margin_start(encGrid, 8); gtk_widget_set_margin_end(encGrid, 8);
	gtk_widget_set_margin_top(encGrid, 6);   gtk_widget_set_margin_bottom(encGrid, 6);
	st.pwField  = gtk_password_entry_new();
	st.pwField2 = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(st.pwField), TRUE);
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(st.pwField2), TRUE);
	gtk_widget_set_size_request(st.pwField, 220, -1);
	gtk_widget_set_size_request(st.pwField2, 220, -1);
	st.encMethod = makePopup();
	st.encNamesCheck = gtk_check_button_new_with_label("Encrypt file names");
	gtk_grid_attach(GTK_GRID(encGrid), label("Enter password:"),    0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), st.pwField,                  1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), label("Reenter password:"),  0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), st.pwField2,                 1, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), label("Encryption method:"), 0, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), st.encMethod.dd,             1, 2, 1, 1);
	gtk_grid_attach(GTK_GRID(encGrid), st.encNamesCheck,            1, 3, 1, 1);
	gtk_frame_set_child(GTK_FRAME(encFrame), encGrid);
	gtk_box_append(GTK_BOX(rightCol), encFrame);

	dlgButtons(outer, &st.d);

	// change wiring (notify::selected on the dropdowns)
	auto onSel = [](GObject* obj, GParamSpec*, gpointer u) {
		AddState* st = (AddState*)u;
		if (st->building) return;
		if (obj == G_OBJECT(st->fmt.dd))         { st->building = true; addFormatChanged(st); st->building = false; }
		else if (obj == G_OBJECT(st->level.dd) ||
		         obj == G_OBJECT(st->method.dd)) { st->building = true; addMethodChanged(st); st->building = false; }
		else if (obj == G_OBJECT(st->dict.dd))   addUpdateMem(st);
	};
	g_signal_connect(st.fmt.dd,    "notify::selected", G_CALLBACK(+onSel), &st);
	g_signal_connect(st.level.dd,  "notify::selected", G_CALLBACK(+onSel), &st);
	g_signal_connect(st.method.dd, "notify::selected", G_CALLBACK(+onSel), &st);
	g_signal_connect(st.dict.dd,   "notify::selected", G_CALLBACK(+onSel), &st);

	st.building = true; addFormatChanged(&st); st.building = false;

	// run — with the password-match check on OK (macOS re-prompts instead of closing)
	for (;;) {
		dlgRun(&st.d);
		if (!st.d.ok) return o;   // cancelled
		std::string pw1 = gtk_editable_get_text(GTK_EDITABLE(st.pwField));
		std::string pw2 = gtk_editable_get_text(GTK_EDITABLE(st.pwField2));
		if (gtk_widget_get_sensitive(st.pwField) && pw1 != pw2) {
			alert("Passwords do not match", "Re-enter the same password in both fields.");
			st.d.ok = false;
			continue;   // dlgRun re-presents the (closed, not destroyed) window
		}
		break;
	}

	std::string path = trimWs(gtk_editable_get_text(GTK_EDITABLE(st.pathField)));
	if (path.empty()) return o;
	o.ok = true;
	o.archivePath  = path;
	o.format       = st.fmt.title();
	o.level        = st.level.valueInt();
	o.method       = st.method.enabled() ? st.method.title() : "";
	o.dict         = st.dict.valueU64();
	o.wordSize     = st.word.valueInt();
	o.solid        = st.solid.enabled() ? st.solid.value() : "";
	o.threads      = st.threads.valueInt();
	o.memusePercent= st.mem.enabled() ? st.mem.value() : "";
	o.password     = gtk_widget_get_sensitive(st.pwField)
	               ? std::string(gtk_editable_get_text(GTK_EDITABLE(st.pwField))) : "";
	o.encMethod    = (!o.password.empty()) ? st.encMethod.value() : "";
	o.encryptNames = gtk_check_button_get_active(GTK_CHECK_BUTTON(st.encNamesCheck));
	o.pathMode     = st.pathModeP.index();
	o.updateMode   = st.update.index();
	o.createSFX    = gtk_check_button_get_active(GTK_CHECK_BUTTON(st.sfxCheck));
	o.compressShared = gtk_check_button_get_active(GTK_CHECK_BUTTON(st.sharedCheck));
	o.deleteAfter  = gtk_check_button_get_active(GTK_CHECK_BUTTON(st.deleteCheck));
	o.splitVolume  = trimWs(gtk_editable_get_text(GTK_EDITABLE(st.splitField)));
	o.extraParams  = trimWs(gtk_editable_get_text(GTK_EDITABLE(st.paramsField)));
	return o;
}

// ════════════════════════════════════════════════════════════════════════════
// Extract
// ════════════════════════════════════════════════════════════════════════════
NZExtractOptions runExtractForArchive(const std::string& archivePath) {
	NZExtractOptions o;
	if (archivePath.empty()) return o;
	std::string dir = dirName(archivePath);
	std::string sub = stripExt(baseName(archivePath));

	Dlg d;
	GtkWidget* outer = dlgNew(&d, "Extract");
	DlgGuard guard(&d);

	GtkWidget* destRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(destRow), leftLabel("Extract to:"));
	GtkWidget* destField = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(destField), dir.c_str());
	gtk_widget_set_size_request(destField, 380, -1);
	gtk_widget_set_hexpand(destField, TRUE);
	gtk_box_append(GTK_BOX(destRow), destField);
	GtkWidget* browse = gtk_button_new_with_label("Browse…");
	g_object_set_data(G_OBJECT(browse), "field", destField);
	g_signal_connect(browse, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
		GtkWidget* field = (GtkWidget*)g_object_get_data(G_OBJECT(b), "field");
		std::string got = chooseFolder(gtk_editable_get_text(GTK_EDITABLE(field)));
		if (!got.empty()) gtk_editable_set_text(GTK_EDITABLE(field), got.c_str());
	}), nullptr);
	gtk_box_append(GTK_BOX(destRow), browse);
	gtk_box_append(GTK_BOX(outer), destRow);

	std::string subLabel = "Extract into subfolder “" + sub + "”";
	GtkWidget* subfolderCheck = gtk_check_button_new_with_label(subLabel.c_str());
	gtk_check_button_set_active(GTK_CHECK_BUTTON(subfolderCheck), TRUE);
	gtk_box_append(GTK_BOX(outer), subfolderCheck);

	GtkWidget* pmRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(pmRow), leftLabel("Path mode:"));
	Popup pathModeP = makePopup();
	pathModeP.fill({{"Full pathnames","0"},{"No pathnames","1"}});
	gtk_box_append(GTK_BOX(pmRow), pathModeP.dd);
	gtk_box_append(GTK_BOX(outer), pmRow);

	GtkWidget* elimCheck = gtk_check_button_new_with_label("Eliminate duplication of root folder");
	gtk_box_append(GTK_BOX(outer), elimCheck);

	GtkWidget* owRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(owRow), leftLabel("Overwrite mode:"));
	Popup overwriteP = makePopup();
	overwriteP.fill({{"Overwrite without prompt","0"},{"Skip existing files","1"},{"Auto rename","2"}});
	gtk_box_append(GTK_BOX(owRow), overwriteP.dd);
	gtk_box_append(GTK_BOX(outer), owRow);

	GtkWidget* pwRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_box_append(GTK_BOX(pwRow), leftLabel("Password:"));
	GtkWidget* pwField = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(pwField), TRUE);
	gtk_widget_set_size_request(pwField, 240, -1);
	gtk_box_append(GTK_BOX(pwRow), pwField);
	gtk_box_append(GTK_BOX(outer), pwRow);

	dlgButtons(outer, &d, "Extract");
	dlgRun(&d);
	if (!d.ok) return o;

	std::string dest = trimWs(gtk_editable_get_text(GTK_EDITABLE(destField)));
	if (dest.empty()) return o;
	o.ok = true;
	o.destDir       = dest;
	o.intoSubfolder = gtk_check_button_get_active(GTK_CHECK_BUTTON(subfolderCheck));
	o.pathMode      = pathModeP.index();
	o.overwrite     = overwriteP.index();
	o.eliminateRoot = gtk_check_button_get_active(GTK_CHECK_BUTTON(elimCheck));
	o.password      = gtk_editable_get_text(GTK_EDITABLE(pwField));
	return o;
}

// ════════════════════════════════════════════════════════════════════════════
// Info window (checksums, test result)
// ════════════════════════════════════════════════════════════════════════════
void showInfoTitle(const std::string& title, const std::string& text) {
	Dlg d;
	GtkWidget* outer = dlgNew(&d, title.empty() ? "Information" : title.c_str(), /*resizable=*/true);
	DlgGuard guard(&d);
	gtk_window_set_default_size(GTK_WINDOW(d.win), 640, 360);

	GtkWidget* scroll = gtk_scrolled_window_new();
	gtk_widget_set_vexpand(scroll, TRUE);
	GtkWidget* tv = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 10);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 10);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 10);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)),
	                         text.c_str(), (int)text.size());
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), tv);
	gtk_box_append(GTK_BOX(outer), scroll);

	GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_set_halign(row, GTK_ALIGN_END);
	GtkWidget* ok = gtk_button_new_with_label("OK");
	gtk_widget_add_css_class(ok, "suggested-action");
	g_object_set_data(G_OBJECT(ok), "dlg", &d);
	g_signal_connect(ok, "clicked", G_CALLBACK(+[](GtkButton* b, gpointer) {
		dlgClose((Dlg*)g_object_get_data(G_OBJECT(b), "dlg"), true);
	}), nullptr);
	gtk_box_append(GTK_BOX(row), ok);
	gtk_box_append(GTK_BOX(outer), row);

	dlgRun(&d);
}

// ════════════════════════════════════════════════════════════════════════════
// Enter-password prompt
// ════════════════════════════════════════════════════════════════════════════
bool promptPasswordForArchive(const std::string& archiveName, bool wrong, std::string& out) {
	Dlg d;
	GtkWidget* outer = dlgNew(&d, "Password");
	DlgGuard guard(&d);

	std::string prompt = archiveName.empty()
		? std::string("Enter password:")
		: "Enter password for “" + archiveName + "”:";
	gtk_box_append(GTK_BOX(outer), leftLabel(prompt.c_str()));
	if (wrong) {
		GtkWidget* warn = leftLabel("Incorrect password — please try again.");
		gtk_widget_add_css_class(warn, "error");
		gtk_box_append(GTK_BOX(outer), warn);
	}

	GtkWidget* pw = gtk_password_entry_new();
	gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(pw), TRUE);
	gtk_widget_set_size_request(pw, 300, -1);
	// Enter in the entry = OK (GtkPasswordEntry activates on Enter)
	g_object_set_data(G_OBJECT(pw), "dlg", &d);
	g_signal_connect(pw, "activate", G_CALLBACK(+[](GtkPasswordEntry* e, gpointer) {
		dlgClose((Dlg*)g_object_get_data(G_OBJECT(e), "dlg"), true);
	}), nullptr);
	gtk_box_append(GTK_BOX(outer), pw);

	dlgButtons(outer, &d);
	dlgRun(&d);
	if (!d.ok) return false;
	out = gtk_editable_get_text(GTK_EDITABLE(pw));
	return true;
}

// Folder chooser used by the controller ("Extract…" → pick destination).
std::string chooseExtractFolder(const std::string& startDir) {
	return chooseFolder(startDir);
}

// Open-file chooser used by the controller ("Open Archive…").
std::string chooseOpenFile(const std::string& startDir) {
	GtkFileDialog* fd = gtk_file_dialog_new();
	if (!startDir.empty()) {
		GFile* f = g_file_new_for_path(startDir.c_str());
		gtk_file_dialog_set_initial_folder(fd, f);
		g_object_unref(f);
	}
	FileDlgCtx ctx; ctx.loop = g_main_loop_new(nullptr, FALSE);
	gtk_file_dialog_open(fd, g_parent, nullptr,
		+[](GObject* src, GAsyncResult* res, gpointer u) {
			FileDlgCtx* c = (FileDlgCtx*)u;
			GFile* f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
			if (f) { gchar* p = g_file_get_path(f); if (p) { c->path = p; c->ok = true; g_free(p); } g_object_unref(f); }
			if (g_main_loop_is_running(c->loop)) g_main_loop_quit(c->loop);
		}, &ctx);
	g_main_loop_run(ctx.loop);
	g_main_loop_unref(ctx.loop);
	g_object_unref(fd);
	return ctx.ok ? ctx.path : std::string();
}

} // namespace NextZipDialogs
