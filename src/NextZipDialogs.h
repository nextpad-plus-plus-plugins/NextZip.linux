/*
 * NextZipDialogs.h — modal GTK4 dialogs modeled on the Windows 7-Zip windows
 * (Add to Archive, Extract, Checksum/Info, Enter password). Built
 * programmatically; each returns an options struct (ok=false = cancelled).
 * Linux port of the macOS NextZipDialogs (same fields, same per-format
 * enable/disable and method/dict/word coupling from CompressDialog.cpp).
 *
 * NextZip 2026 (GPL).
 */
#pragma once

#include <gtk/gtk.h>
#include <string>
#include <vector>
#include <cstdint>

// Result of the "Add to Archive" dialog — mirrors SevenZipEngine::CompressOptions
// plus the few UI-only toggles from the Windows dialog.
struct NZAddOptions {
	bool        ok = false;        // false = cancelled
	std::string archivePath;       // full destination path
	std::string format = "7z";     // 7z|zip|tar|gzip|bzip2|xz
	int         level = 5;         // 0,1,3,5,7,9
	std::string method;            // "" = default
	uint64_t    dict = 0;          // bytes; 0 = auto
	int         wordSize = 0;      // 0 = auto
	std::string solid;             // ""|"off"|"on"|"<n>b"
	int         threads = 0;       // 0 = auto
	std::string memusePercent;     // ""|"NN%"
	std::string password;          // "" = none
	std::string encMethod;         // ""|"AES256"|"ZipCrypto"
	bool        encryptNames = false; // 7z only
	int         pathMode = 0;      // 0 relative, 1 full, 2 absolute
	int         updateMode = 0;    // 0 add&replace,1 update,2 freshen,3 sync
	bool        createSFX = false; // UI only (not supported on Linux)
	bool        compressShared = false;
	bool        deleteAfter = false;
	std::string splitVolume;       // raw text (e.g. "100m"); "" = none
	std::string extraParams;       // advanced "name=value …"
};

// Result of the "Extract" dialog.
struct NZExtractOptions {
	bool        ok = false;        // false = cancelled
	std::string destDir;           // directory to extract into
	bool        intoSubfolder = true; // create a folder named after the archive
	int         pathMode = 0;      // 0 = full paths, 1 = no paths (flatten)
	int         overwrite = 0;     // 0 overwrite, 1 skip, 2 auto-rename
	bool        eliminateRoot = false; // drop a shared top-level folder
	std::string password;          // "" = none
};

namespace NextZipDialogs {
// The parent for every modal (the host main window); may be null.
void setParentWindow(GtkWindow* parent);

// ok=false if cancelled. `inputs` = the filesystem paths being compressed.
NZAddOptions runAddForInputs(const std::vector<std::string>& inputs);
// ok=false if cancelled. `archivePath` seeds the default destination.
NZExtractOptions runExtractForArchive(const std::string& archivePath);
// A simple scrollable monospaced info window with an OK button.
void showInfoTitle(const std::string& title, const std::string& text);
// Modal "Enter password" prompt (masked entry + Show Password). Returns true
// with the entered password in `out` (possibly empty), false if cancelled.
// Pass wrong=true on a retry to show the bad-password hint.
bool promptPasswordForArchive(const std::string& archiveName, bool wrong, std::string& out);
// File choosers used by the controller (sync wrappers over GtkFileDialog).
std::string chooseOpenFile(const std::string& startDir);
std::string chooseExtractFolder(const std::string& startDir);
// Simple OK alert and OK/Cancel confirm (nested-loop modal).
void alert(const std::string& title, const std::string& info);
bool confirm(const std::string& title, const std::string& info,
             const std::string& okLabel, const std::string& cancelLabel);
} // namespace NextZipDialogs
