/*
 * A wbsh util, ready to rename. Plain C: nothing here needs a C++
 * compiler, and nothing is linked against the SDK.
 *
 *   msbuild myutil.vcxproj -p:Configuration=Release -p:Platform=x64 \
 *       -p:WbshSdkDir=<folder holding wbshutil.props>
 *
 * or, with CMake and whichever compiler it finds:
 *
 *   cmake -S . -B build -DWBSH_SDK_DIR=<that folder> && cmake --build build
 *
 * Then drop the DLL into %APPDATA%\wbsh\plugins and restart the shell, or
 * `utils load build\myutil.dll` in a running one.
 */

#include "wbshsdk.h"

#include <string.h>

static const WbshUtilInfo kInfo = {
	WBSH_SDK_ABI,
	"myutil",
	"0.1.0",
	"Describe what this util adds, in one line."
};

static const WbshApi* wbsh = NULL;

static int myCommand(void* user, int argc, const char* const* argv) {
	(void)user; (void)argc; (void)argv;

	wbsh->print("myutil: hello from a util\n");
	return 0;
}

static int registerMyCommand(void) {
	WbshCommand command;
	memset(&command, 0, sizeof(command));
	command.size    = sizeof(command);
	command.name    = "myutil";
	command.summary = "One line for `help`.";
	command.usage   = "myutil [args]";
	command.fn      = myCommand;

	return wbsh->register_command(&command);
}

WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void) {
	return &kInfo;
}

WBSH_UTIL_API int wbshUtilLoad(const WbshApi* api) {
	wbsh = api;

	if (api->host == WBSH_HOST_SHELL) {
		registerMyCommand();
	}

	return WBSH_OK;
}

WBSH_UTIL_API void wbshUtilUnload(void) {
	wbsh = NULL;
}
