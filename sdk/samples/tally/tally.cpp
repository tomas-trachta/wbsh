/**
 * @file tally.cpp
 * @brief The two halves of a util agreeing across the process boundary.
 *
 * wbshterm.exe and wbsh.exe are separate processes, so the copy of this
 * DLL the terminal loads and the copy the shell loads share no globals.
 * `tally` counts in the shell; the status bar shows the count in the
 * terminal; the number has to cross between them somehow.
 *
 * It crosses through a named piece of shared memory. Both copies open the
 * same mapping by name in wbshUtilLoad, the command writes a counter into
 * it, and the segment reads the counter straight out of it. A read of
 * one aligned word from a mapped page cannot block, which is what makes
 * it safe on the paint path.
 *
 * The mapping lives in the Local\ namespace, so it is one per logged-in
 * session and needs no rights beyond the user's own.
 */

#include "wbshsdk.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstdio>
#include <cstring>

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"tally",
	"1.0.0",
	"A counter kept in the shell, shown in the terminal's status bar."
};

static const wchar_t* const kMappingName = L"Local\\wbsh-sample-tally";

static const WbshApi* wbsh = nullptr;

static HANDLE g_mapping = nullptr;
static volatile LONG* g_count = nullptr;
static char g_segment[32] = "";

// CreateFileMapping on a name that already exists opens the existing
// one, so whichever host loads first makes it and the other joins it.
// The page comes zeroed the first time.
static bool openSharedCounter(void) {
	g_mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
		0, sizeof(LONG), kMappingName);
	if (g_mapping == nullptr) return false;

	void* view = ::MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
	if (view == nullptr) {
		::CloseHandle(g_mapping);
		g_mapping = nullptr;
		return false;
	}

	g_count = static_cast<volatile LONG*>(view);
	return true;
}

static void closeSharedCounter(void) {
	if (g_count != nullptr) ::UnmapViewOfFile(const_cast<LONG*>(g_count));
	if (g_mapping != nullptr) ::CloseHandle(g_mapping);

	g_count = nullptr;
	g_mapping = nullptr;
}

static int printCount(void) {
	char line[64];
	std::snprintf(line, sizeof(line), "%ld\n", *g_count);
	wbsh->print(line);
	return 0;
}

static int tallyCommand(void* user, int argc, const char* const* argv) {
	(void)user;
	if (g_count == nullptr) {
		wbsh->print_error("tally: the shared counter could not be opened\n");
		return 1;
	}

	if (argc > 1 && std::strcmp(argv[1], "--show") == 0) return printCount();

	if (argc > 1 && std::strcmp(argv[1], "--reset") == 0) {
		::InterlockedExchange(g_count, 0);
		return 0;
	}

	if (argc > 1) {
		wbsh->print_error("usage: tally [--show | --reset]\n");
		return 2;
	}

	::InterlockedIncrement(g_count);
	return printCount();
}

// Formatting into a buffer the util owns is the whole of the work here,
// and nothing in it waits on anything.
static const char* tallySegment(void* user) {
	(void)user;
	if (g_count == nullptr) return nullptr;

	std::snprintf(g_segment, sizeof(g_segment), "tally: %ld", *g_count);
	return g_segment;
}

static int registerTally(void) {
	WbshCommand command;
	std::memset(&command, 0, sizeof(command));
	command.size    = sizeof(command);
	command.name    = "tally";
	command.summary = "Counts up; the terminal's status bar shows the count.";
	command.usage   = "tally [--show | --reset]";
	command.fn      = tallyCommand;

	return wbsh->register_command(&command);
}

extern "C" {

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

WBSH_UTIL_API int wbshUtilLoad(const WbshApi* api) {
	wbsh = api;
	openSharedCounter();

	if (api->host == WBSH_HOST_SHELL) registerTally();
	if (api->host == WBSH_HOST_TERMINAL) api->register_segment("tally", tallySegment, nullptr);

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
	closeSharedCounter();
	wbsh = nullptr;
}

}  /* extern "C" */
