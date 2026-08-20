// Headless validation of composite tgz create (tar pass + gzip pass) and of the
// pax binary-xattr tolerance patch (deps/patches/0001) that keeps macOS-created
// tars/tgz writable. Run: test_tgz <7z.so> <inputDir> <writable outDir>
#include "SevenZipEngine.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <dirent.h>

static bool ex(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0; }
static long long fsize(const std::string& p) { struct stat s; return ::stat(p.c_str(), &s) == 0 ? (long long)s.st_size : -1; }
static bool writeFile(const std::string& p, const std::string& c) {
	FILE* f = fopen(p.c_str(), "wb"); if (!f) return false;
	fwrite(c.data(), 1, c.size(), f); fclose(f); return true;
}
static std::string readFile(const std::string& p) {
	FILE* f = fopen(p.c_str(), "rb"); if (!f) return "";
	std::string s; char b[4096]; size_t r;
	while ((r = fread(b, 1, sizeof b, f)) > 0) s.append(b, r);
	fclose(f); return s;
}
static std::string loneFile(const std::string& dir) {
	DIR* d = opendir(dir.c_str()); if (!d) return "";
	std::string only;
	for (dirent* e; (e = readdir(d)); ) {
		std::string n = e->d_name;
		if (n == "." || n == "..") continue;
		if (!only.empty()) { only = ""; break; }
		only = n;
	}
	closedir(d);
	return only.empty() ? "" : dir + "/" + only;
}

// ── minimal tar writer with a pax record carrying a NUL-bearing binary xattr ──
// (what macOS bsdtar emits for com.apple.provenance — the case patch 0001 fixes)
static void tarHeader(std::string& out, const char* name, char type, size_t size) {
	char h[512]; memset(h, 0, sizeof h);
	snprintf(h,       100, "%s", name);
	snprintf(h + 100,   8, "%07o", 0644);
	snprintf(h + 108,   8, "%07o", 1000);
	snprintf(h + 116,   8, "%07o", 1000);
	snprintf(h + 124,  12, "%011llo", (unsigned long long)size);
	snprintf(h + 136,  12, "%011llo", 0ULL);
	memset(h + 148, ' ', 8);                       // chksum computed over spaces
	h[156] = type;
	memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
	unsigned sum = 0; for (int i = 0; i < 512; i++) sum += (unsigned char)h[i];
	snprintf(h + 148, 8, "%06o", sum); h[154] = '\0'; h[155] = ' ';
	out.append(h, 512);
}
static void tarData(std::string& out, const std::string& d) {
	out += d;
	if (d.size() % 512) out.append(512 - d.size() % 512, '\0');
}
static std::string paxRecord(const std::string& key, const std::string& val) {
	// "N key=value\n" where N counts the whole record including itself
	size_t body = 1 + key.size() + 1 + val.size() + 1;   // ' ' key '=' val '\n'
	size_t len = body + 1;                                // 1-digit guess
	char n[16];
	for (;;) { size_t d = snprintf(n, sizeof n, "%zu", len); if (d + body == len) break; len = d + body; }
	return std::string(n) + " " + key + "=" + val + "\n";
}
static bool writePaxXattrTar(const std::string& path) {
	std::string binval("\x01\x64", 2); binval += '\0'; binval += '\x02';   // NUL inside
	std::string pax = paxRecord("SCHILY.xattr.com.apple.provenance", binval);
	std::string t;
	tarHeader(t, "PaxHeaders.0/keep.txt", 'x', pax.size()); tarData(t, pax);
	tarHeader(t, "keep.txt", '0', 10);                      tarData(t, "keep-data\n");
	tarHeader(t, "replace.txt", '0', 9);                    tarData(t, "old-data\n");
	t.append(1024, '\0');
	return writeFile(path, t);
}

int main(int argc, char** argv) {
	if (argc < 4) { printf("usage: %s 7z.so inputDir outDir\n", argv[0]); return 2; }
	std::string eng = argv[1], in = argv[2], out = argv[3];

	// 1) composite create: site.tgz = tar pass (member <base>.tar) + gzip pass
	std::string dest = out + "/site.tgz";
	{
		NextZipEngine e; e.setEnginePath(eng);
		NextZipEngine::CompressOptions o; o.format = "tgz"; o.level = 5;
		if (!e.compress(dest, o, { in })) { printf("FAIL tgz compress: %s\n", e.error().c_str()); return 1; }
	}
	NextZipEngine gz; gz.setEnginePath(eng);
	if (!gz.open(dest)) { printf("FAIL open tgz: %s\n", gz.error().c_str()); return 1; }
	if (gz.format() != "gzip" || gz.entries().size() != 1) {
		printf("FAIL: expected gzip with 1 entry, got %s/%zu\n", gz.format().c_str(), gz.entries().size()); return 1;
	}
	if (gz.entries()[0].path != "site.tar") {
		printf("FAIL: gzip member name '%s' (want site.tar)\n", gz.entries()[0].path.c_str()); return 1;
	}
	printf("tgz create OK → gzip{%s}\n", gz.entries()[0].path.c_str());

	// 2) descent + byte round-trip of every input file
	std::string innerDir = out + "/inner";
	if (!gz.extract({0}, innerDir)) { printf("FAIL extract inner: %s\n", gz.error().c_str()); return 1; }
	std::string innerTar = loneFile(innerDir);
	NextZipEngine tar; tar.setEnginePath(eng);
	if (!tar.open(innerTar) || tar.format() != "tar") { printf("FAIL open inner tar\n"); return 1; }
	std::string xdir = out + "/roundtrip";
	if (!tar.extract({}, xdir)) { printf("FAIL extract tar: %s\n", tar.error().c_str()); return 1; }
	std::string base = in; if (size_t s = base.find_last_of('/'); s != std::string::npos) base = base.substr(s + 1);
	int checked = 0;
	for (const char* rel : { "a.txt", "sub/b.txt", "blob.bin" }) {
		std::string a = readFile(in + "/" + rel), b = readFile(xdir + "/" + base + "/" + rel);
		if (a.empty() || a != b) { printf("FAIL round-trip mismatch: %s\n", rel); return 1; }
		checked++;
	}
	printf("round-trip OK (%d files byte-identical, %zu entries)\n", checked, tar.entries().size());

	// 3) level is plumbed through the gzip pass
	{
		std::string rep(200000, 'x'); writeFile(out + "/rep.txt", rep);
		NextZipEngine e1; e1.setEnginePath(eng);
		NextZipEngine::CompressOptions o1; o1.format = "tgz"; o1.level = 1;
		if (!e1.compress(out + "/l1.tgz", o1, { out + "/rep.txt" })) { printf("FAIL l1: %s\n", e1.error().c_str()); return 1; }
		NextZipEngine e9; e9.setEnginePath(eng);
		NextZipEngine::CompressOptions o9; o9.format = "tgz"; o9.level = 9;
		if (!e9.compress(out + "/l9.tgz", o9, { out + "/rep.txt" })) { printf("FAIL l9: %s\n", e9.error().c_str()); return 1; }
		long long s1 = fsize(out + "/l1.tgz"), s9 = fsize(out + "/l9.tgz");
		if (s1 <= 0 || s9 <= 0 || s9 > s1) { printf("FAIL level plumbing: l1=%lld l9=%lld\n", s1, s9); return 1; }
		printf("level plumbing OK (l1=%lld > l9=%lld)\n", s1, s9);
	}

	// 4) deleteAfter: inputs removed only after both passes succeed
	{
		std::string tmpIn = out + "/della.txt"; writeFile(tmpIn, "bye\n");
		NextZipEngine e; e.setEnginePath(eng);
		NextZipEngine::CompressOptions o; o.format = "tgz"; o.level = 5; o.deleteAfter = true;
		if (!e.compress(out + "/del.tgz", o, { tmpIn })) { printf("FAIL deleteAfter compress: %s\n", e.error().c_str()); return 1; }
		if (ex(tmpIn)) { printf("FAIL: deleteAfter left input behind\n"); return 1; }
		printf("deleteAfter OK\n");
	}

	// 5) pax patch: a tar with a NUL-bearing binary xattr record must stay
	//    listable AND writable (upstream 7-Zip flags it corrupt → E_NOTIMPL)
	{
		std::string paxTar = out + "/pax.tar";
		if (!writePaxXattrTar(paxTar)) { printf("FAIL write pax tar\n"); return 1; }
		NextZipEngine e; e.setEnginePath(eng);
		if (!e.open(paxTar)) { printf("FAIL open pax tar: %s\n", e.error().c_str()); return 1; }
		size_t files = 0; for (auto& en : e.entries()) if (!en.isDir) files++;
		if (files != 2) { printf("FAIL: pax tar lists %zu files (want 2)\n", files); return 1; }
		std::string nf = out + "/new.txt"; writeFile(nf, "NEW-DATA\n");
		if (!e.updateFile("replace.txt", nf)) { printf("FAIL pax updateFile: %s (patch 0001 missing?)\n", e.error().c_str()); return 1; }
		std::string raw = readFile(paxTar);
		if (raw.find("com.apple.provenance") == std::string::npos) {
			printf("FAIL: kept entry lost its xattr pax record on update\n"); return 1;
		}
		NextZipEngine r; r.setEnginePath(eng);
		if (!r.open(paxTar)) { printf("FAIL reopen pax tar: %s\n", r.error().c_str()); return 1; }
		bool sawNew = false;
		for (auto& en : r.entries()) if (en.path == "replace.txt" && en.size == 9) sawNew = true;
		if (!sawNew) { printf("FAIL: replaced entry wrong after update\n"); return 1; }
		printf("pax xattr tar OK (2 entries, update succeeded, xattr record survived)\n");
	}

	printf("ALL-OK\n");
	return 0;
}
