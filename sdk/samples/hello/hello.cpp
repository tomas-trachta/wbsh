/**
 * @file hello.cpp
 * @brief A worked example of a wbsh util: one command and one status
 *        segment, in one DLL.
 *
 * Build it, drop the DLL in the plugins folder beside wbsh.exe, and both
 * halves appear: `hello` becomes a command in the shell, and the terminal
 * grows a segment in its status bar. Neither host needs rebuilding.
 *
 * The same DLL is loaded once by each host, so wbshUtilLoad runs twice
 * and asks which host it is in before registering anything.
 */

#include "wbshsdk.h"

#include <cstdio>
#include <cstring>

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"hello",
	"1.0.0",
	"A worked example: one command and one status segment."
};

// The segment is asked for its text while the terminal paints, so it can
// only hand back something that is already sitting there.
static char g_segment[64] = "hello: 0";
static unsigned g_greetings = 0;

static void rememberGreeting(void) {
	++g_greetings;
	std::snprintf(g_segment, sizeof(g_segment), "hello: %u", g_greetings);
}

static int greet(const char* who) {
	char line[512];
	std::snprintf(line, sizeof(line), "Hello, %s!\n", who);
	wbshPrint(line);
	return 0;
}

static int reportWhere(void) {
	const char* directory = wbshWorkingDirectory();
	if (directory == nullptr) return 1;

	char line[1024];
	std::snprintf(line, sizeof(line), "You are in %s\n", directory);
	wbshPrint(line);
	return 0;
}

// argv[0] is the command's own name, so the options start at argv[1] the
// way they do in main().
static int helloCommand(void* user, int argc, const char* const* argv) {
	(void)user;
	rememberGreeting();

	if (argc > 1 && std::strcmp(argv[1], "--where") == 0) return reportWhere();

	if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
		wbshPrint("usage: hello [name] | hello --where\n");
		return 0;
	}

	if (argc > 1) return greet(argv[1]);

	const char* user_name = wbshVariable("USERNAME");
	return greet(user_name != nullptr ? user_name : "world");
}

static const char* helloSegment(void* user) {
	(void)user;
	return g_segment;
}

extern "C" {

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

// A host that does not offer one of these answers WBSH_ERR_UNSUPPORTED,
// which is not a reason to fail: the util simply has nothing to add here.
WBSH_UTIL_API int wbshUtilLoad(WbshHostKind host) {
	if (host == WBSH_HOST_SHELL) wbshRegisterCommand("hello", helloCommand, nullptr);
	if (host == WBSH_HOST_TERMINAL) wbshRegisterStatusSegment("hello", helloSegment, nullptr);

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
}

}  /* extern "C" */
