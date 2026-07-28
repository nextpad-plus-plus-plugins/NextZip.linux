/*
 * NextZipHost.h — the tiny host abstraction that lets ONE NextZipController
 * (the shared archive-manager view + logic) stay host-agnostic.
 *
 * The controller never talks to a host directly; it calls through this
 * interface. On Linux one implementation exists — the Nextpad++ plugin shell
 * (NextZipPlugin.cpp): open-in-editor via NPPM_DOOPEN, current-file via
 * NPPM_GETFULLCURRENTPATH, the dock panel register/show/hide.
 * (On macOS a second shell backed the standalone NextZip.app.)
 *
 * NextZip 2026 (GPL).
 */
#pragma once

#include <string>

struct NextZipHost {
	virtual ~NextZipHost() = default;

	// Open a file the controller just extracted from the archive to a temp path.
	// `stablePath` points into the controller's opened-temps map, so it stays
	// valid past this call. (The Linux host's NPPM_DOOPEN opens synchronously,
	// but the stable-pointer contract is kept — it costs nothing and stays
	// faithful to the macOS shell.)
	virtual void openExtractedFile(const std::string& displayPath, const char* stablePath) = 0;

	// The path to treat as "the current file" for "Open Current File as
	// Archive" — the active editor tab. Empty = none.
	virtual std::string currentFilePath() = 0;

	// Ensure the archive-manager panel is visible and frontmost (called right
	// after an archive is opened): register if needed + NPPM_DMM_SHOWPANEL.
	virtual void revealPanel() = 0;
};
