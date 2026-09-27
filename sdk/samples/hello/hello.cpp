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

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif /* WIN32_LEAN_AND_MEAN */
#include <windows.h>

#include <cstdio>
#include <cstring>

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"hello",
	"1.0.0",
	"A worked example: one command and one status segment."
};

static const char* const kOptions[] = {
	"--help", "--set", "--wait", "--where", nullptr
};

// The host's functions arrive with wbshUtilLoad and stay valid for as
// long as the DLL is loaded, so one global is the whole of the plumbing.
static const WbshApi* wbsh = nullptr;

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
	wbsh->print(line);
	return 0;
}

static int reportWhere(void) {
	const char* directory = wbsh->working_directory();
	if (directory == nullptr) return 1;

	char line[1024];
	std::snprintf(line, sizeof(line), "You are in %s\n", directory);
	wbsh->print(line);
	return 0;
}

// A command that runs for a while asks the host now and then whether
// Ctrl+C has been pressed, and answers the way a killed program would.
static int waitUntilCancelled(void) {
	wbsh->print("waiting; press Ctrl+C\n");

	while (wbsh->cancelled() == 0) ::Sleep(50);

	wbsh->print("cancelled\n");
	return 130;
}

static int setVariable(int argc, const char* const* argv) {
	if (argc < 4) {
		wbsh->print_error("hello: --set needs a name and a value\n");
		return 2;
	}

	return wbsh->set_variable(argv[2], argv[3]) == WBSH_OK ? 0 : 1;
}

// argv[0] is the command's own name, so the options start at argv[1] the
// way they do in main().
static int helloCommand(void* user, int argc, const char* const* argv) {
	(void)user;
	rememberGreeting();

	if (argc > 1 && std::strcmp(argv[1], "--where") == 0) return reportWhere();
	if (argc > 1 && std::strcmp(argv[1], "--wait") == 0) return waitUntilCancelled();
	if (argc > 1 && std::strcmp(argv[1], "--set") == 0) return setVariable(argc, argv);

	if (argc > 1 && std::strcmp(argv[1], "--help") == 0) {
		wbsh->print("usage: hello [name] | hello --where | hello --set NAME VALUE\n");
		return 0;
	}

	if (argc > 1) return greet(argv[1]);

	const char* user_name = wbsh->variable("USERNAME");
	return greet(user_name != nullptr ? user_name : "world");
}

// Tab on the first argument offers the options; the host keeps only the
// ones that start with what was typed, so every option is offered here.
static void helloComplete(void* user, int argc, const char* const* argv,
		WbshCompletion* completion) {
	(void)user; (void)argv;
	if (argc != 2) return;

	for (const char* const* option = kOptions; *option != nullptr; ++option) {
		wbsh->complete_add(completion, *option);
	}
}

static const char* helloSegment(void* user) {
	(void)user;
	return g_segment;
}

static int registerHello(void) {
	WbshCommand command;
	std::memset(&command, 0, sizeof(command));
	command.size     = sizeof(command);
	command.name     = "hello";
	command.summary  = "Says hello, and shows what a util can do.";
	command.usage    = "hello [name] | hello --where | hello --set NAME VALUE | hello --wait";
	command.fn       = helloCommand;
	command.complete = helloComplete;

	return wbsh->register_command(&command);
}

extern "C" {

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

// A host that does not offer one of these answers WBSH_ERR_UNSUPPORTED,
// which is not a reason to fail: the util simply has nothing to add here.
WBSH_UTIL_API int wbshUtilLoad(const WbshApi* api) {
	wbsh = api;

	if (api->host == WBSH_HOST_SHELL) registerHello();
	if (api->host == WBSH_HOST_TERMINAL) api->register_segment("hello", helloSegment, nullptr);

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
	wbsh = nullptr;
}

}  /* extern "C" */
