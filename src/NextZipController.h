/*
 * NextZipController.h — the NextZip archive-manager panel controller (GTK4).
 * Linux port of the macOS NextZipController: opens an archive via
 * NextZipEngine and shows it in a two-pane File-Manager — a filesystem
 * browser on top (single-click an archive to view it) and the archive
 * contents below (breadcrumb, drill into folders, Extract/Test/Info,
 * open-in-editor with save-back, nested .tar.gz descent).
 *
 * NextZip (Nextpad++ archive plugin) 2026 (GPL).
 */
#pragma once

#include <gtk/gtk.h>
#include <string>
#include "NextZipHost.h"

class NextZipController {
public:
	NextZipController();
	~NextZipController();

	// The host bridge (the plugin shell). Not owned.
	void setHost(NextZipHost* host) { m_host = host; }

	// The archive-manager view (filesystem browser on top, archive contents
	// below). Built lazily on first access. The plugin registers it as a dock
	// panel; the host owns the widget after registration.
	GtkWidget* panelView();

	// The outermost real archive file currently shown (empty if none yet).
	std::string currentArchivePath() const;

	void showOpenPanel();                       // "Open Archive…" menu command
	void openArchiveAtPath(const std::string& path, bool quiet = false);
	void openCurrentEditorFile();               // open the host's current file as an archive
	void handleFileSaved(const std::string& path); // write a temp we extracted back into its archive
	void showAbout();

private:
	NextZipHost* m_host = nullptr;
	struct Impl;
	Impl* d;
};
