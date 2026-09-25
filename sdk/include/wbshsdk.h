#pragma once

/**
 * @file wbshsdk.h
 * @brief The interface a third-party wbsh util is written against.
 *
 * A util is a DLL. It exports the three entry points below, calls the
 * register functions to say what it adds, and is dropped into a plugins
 * folder; nothing needs rebuilding to install one.
 *
 * Two hosts load utils, and a util is told which one it is in:
 *
 *   - wbsh.exe registers commands, so a util's command works in scripts
 *     and pipelines like any other.
 *   - wbshterm.exe registers terminal features, which only exist inside
 *     the window.
 *
 * One DLL may do both. It is loaded once per host, so wbshUtilLoad runs
 * twice when the terminal is running the shell, with a different host
 * each time; ask wbshHost() rather than assuming.
 *
 * Everything crossing this boundary is C: plain types, no exceptions, no
 * standard library objects. Strings are UTF-8 and NUL-terminated unless a
 * length is given, and any string handed to a util belongs to the host
 * and is only valid until the call returns.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(WBSH_SDK_BUILD)
#    define WBSH_SDK_API __declspec(dllexport)
#  else
#    define WBSH_SDK_API __declspec(dllimport)
#  endif /* WBSH_SDK_BUILD */
#  define WBSH_UTIL_API __declspec(dllexport)
#else /* _WIN32 */
#  define WBSH_SDK_API
#  define WBSH_UTIL_API
#endif /* _WIN32 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief The number a util is compiled against.
 *
 * A host refuses a util whose abi field is not exactly this. Bump it
 * whenever anything below changes shape, never for an addition at the
 * end of a struct that carries its own size.
 */
#define WBSH_SDK_ABI 1u

#define WBSH_OK               0
#define WBSH_ERR_NO_HOST     (-1)   /**< called before a host was bound */
#define WBSH_ERR_ABI         (-2)   /**< the util was built for another ABI */
#define WBSH_ERR_UNSUPPORTED (-3)   /**< this host does not offer that */
#define WBSH_ERR_BAD_NAME    (-4)   /**< empty, too long, or has a space */
#define WBSH_ERR_TAKEN       (-5)   /**< something already has that name */
#define WBSH_ERR_NO_TERMINAL (-6)   /**< no console to talk to, or it went away */

typedef enum WbshHostKind {
	WBSH_HOST_NONE     = 0,
	WBSH_HOST_SHELL    = 1,
	WBSH_HOST_TERMINAL = 2
} WbshHostKind;

/** What a util says about itself, before any of it is run. */
typedef struct WbshUtilInfo {
	uint32_t    abi;       /**< must be WBSH_SDK_ABI */
	const char* name;      /**< short, no spaces: "weather" */
	const char* version;   /**< free text: "1.2.0" */
	const char* summary;   /**< one line, shown by `utils` */
} WbshUtilInfo;

/**
 * @brief A command the shell can run. @p argv[0] is the command's name.
 *
 * The return value is the exit status, so 0 means success as it does
 * everywhere else in a shell.
 */
typedef int (*WbshCommandFn)(void* user, int argc, const char* const* argv);

/**
 * @brief Text for the terminal's status bar, or NULL for nothing.
 *
 * Called often, on the thread that paints, so it must return quickly and
 * must not block. The returned string belongs to the util and has to
 * outlive the call.
 */
typedef const char* (*WbshSegmentFn)(void* user);

/* ---- what a util exports ------------------------------------------- */

/**
 * @brief Identifies the util. Called before anything else is.
 *
 * Must not depend on wbshUtilLoad having run, and must return the same
 * pointer every time; a host may ask without ever loading the util.
 */
WBSH_UTIL_API const WbshUtilInfo* wbshUtilDescribe(void);

/**
 * @brief Registers whatever this util adds to @p host. 0 on success.
 *
 * Returning anything else unloads the util, so a util that has nothing
 * for this host should still return WBSH_OK and register nothing.
 */
WBSH_UTIL_API int wbshUtilLoad(WbshHostKind host);

/** Called before the DLL is let go. Optional. */
WBSH_UTIL_API void wbshUtilUnload(void);

/* ---- what a util may call ------------------------------------------ */

/** Which host this util is loaded in, or WBSH_HOST_NONE outside one. */
WBSH_SDK_API WbshHostKind wbshHost(void);

/** The SDK's own version, for a util that wants to report it. */
WBSH_SDK_API const char* wbshSdkVersion(void);

/**
 * @brief Adds a command, shadowing an external program of that name.
 *
 * Only the shell host has commands; the terminal answers
 * WBSH_ERR_UNSUPPORTED, which is not an error worth failing a load over.
 */
WBSH_SDK_API int wbshRegisterCommand(const char* name, WbshCommandFn fn, void* user);

/** Adds a status-bar segment. Terminal host only. */
WBSH_SDK_API int wbshRegisterStatusSegment(const char* name, WbshSegmentFn fn, void* user);

WBSH_SDK_API void wbshWriteOut(const char* bytes, size_t length);
WBSH_SDK_API void wbshWriteErr(const char* bytes, size_t length);

/** wbshWriteOut for a NUL-terminated string. Adds no newline. */
WBSH_SDK_API void wbshPrint(const char* text);
WBSH_SDK_API void wbshPrintError(const char* text);

/** The host's working directory, or NULL. Valid until the call returns. */
WBSH_SDK_API const char* wbshWorkingDirectory(void);

/** A shell or environment variable, or NULL when it is not set. */
WBSH_SDK_API const char* wbshVariable(const char* name);

/* ---- the terminal -------------------------------------------------- */

/**
 * @brief The console this process is attached to, taken over for an
 *        interactive interface.
 *
 * Opening it puts the keyboard in raw mode and turns on escape-sequence
 * processing for output; closing it puts both back. It is the console
 * device itself and not stdin or stdout, so `ls | mypicker | xargs`
 * still gets keys and still draws, and a picked line still goes down the
 * pipe through wbshPrint.
 *
 * It exists only where a console does. Inside wbshterm.exe there is none,
 * and wbshTerminalOpen returns NULL.
 */
typedef struct WbshTerminal WbshTerminal;

typedef enum WbshKeyKind {
	WBSH_KEY_NONE      = 0,
	WBSH_KEY_CHAR      = 1,    /**< text holds the character, in UTF-8 */
	WBSH_KEY_ENTER     = 2,
	WBSH_KEY_ESCAPE    = 3,
	WBSH_KEY_BACKSPACE = 4,
	WBSH_KEY_TAB       = 5,
	WBSH_KEY_DELETE    = 6,
	WBSH_KEY_INSERT    = 7,
	WBSH_KEY_UP        = 8,
	WBSH_KEY_DOWN      = 9,
	WBSH_KEY_LEFT      = 10,
	WBSH_KEY_RIGHT     = 11,
	WBSH_KEY_HOME      = 12,
	WBSH_KEY_END       = 13,
	WBSH_KEY_PAGE_UP   = 14,
	WBSH_KEY_PAGE_DOWN = 15,
	WBSH_KEY_F1        = 16,   /**< F2 is WBSH_KEY_F1 + 1, and so on to F12 */
	WBSH_KEY_F12       = 27,
	WBSH_KEY_RESIZE    = 28    /**< the window changed size; ask wbshTerminalSize */
} WbshKeyKind;

#define WBSH_MOD_SHIFT 1u
#define WBSH_MOD_CTRL  2u
#define WBSH_MOD_ALT   4u

/**
 * @brief One keystroke, decoded.
 *
 * For WBSH_KEY_CHAR, modifiers never carries WBSH_MOD_SHIFT: shift is
 * already in the character. Ctrl+C is "c" with WBSH_MOD_CTRL. AltGr is
 * not a modifier at all; it produces the layout's character on its own.
 */
typedef struct WbshKey {
	WbshKeyKind kind;
	uint32_t    modifiers;
	char        text[8];   /**< NUL-terminated; empty unless kind is WBSH_KEY_CHAR */
} WbshKey;

/** 1 when @p fd (0, 1 or 2) is the console rather than a pipe or file. */
WBSH_SDK_API int wbshIsTerminal(int fd);

/** NULL when there is no console. Pair every open with a close. */
WBSH_SDK_API WbshTerminal* wbshTerminalOpen(void);
WBSH_SDK_API void wbshTerminalClose(WbshTerminal* terminal);

/** The visible window in cells. Either out pointer may be NULL. */
WBSH_SDK_API int wbshTerminalSize(WbshTerminal* terminal, int* out_columns, int* out_rows);

/** Bytes straight to the screen, escape sequences included. */
WBSH_SDK_API void wbshTerminalWrite(WbshTerminal* terminal, const char* bytes, size_t length);
WBSH_SDK_API void wbshTerminalPrint(WbshTerminal* terminal, const char* text);

/**
 * @brief Waits for a key. 1 when @p out_key holds one, 0 on timeout,
 *        negative when the console is gone.
 *
 * @p timeout_ms below zero waits forever; zero only takes what is already
 * queued. Ctrl+C is delivered as a key and does not end the process.
 */
WBSH_SDK_API int wbshTerminalReadKey(WbshTerminal* terminal, WbshKey* out_key, int timeout_ms);

/* ---- what a host calls --------------------------------------------- */

/**
 * @brief The functions a host lends the SDK. Hosts only.
 *
 * @p size is sizeof(WbshHostApi) as the host saw it, so a newer SDK can
 * tell which of the trailing members an older host actually filled in.
 */
typedef struct WbshHostApi {
	uint32_t     size;
	uint32_t     abi;
	WbshHostKind kind;
	void*        context;

	int  (*register_command)(void* context, const char* name, WbshCommandFn fn, void* user);
	int  (*register_segment)(void* context, const char* name, WbshSegmentFn fn, void* user);
	void (*write_out)(void* context, const char* bytes, size_t length);
	void (*write_err)(void* context, const char* bytes, size_t length);
	const char* (*working_directory)(void* context);
	const char* (*variable)(void* context, const char* name);
} WbshHostApi;

/** Where a util's complaints go. Hosts only. */
typedef void (*WbshLogFn)(void* context, const char* message);

/** Hands the SDK the host's functions. Hosts only, once, before loading. */
WBSH_SDK_API int wbshSdkBindHost(const WbshHostApi* api);

/**
 * @brief Loads every .dll in @p directory. Hosts only.
 *
 * A util that will not load is reported through @p log and skipped, so
 * one bad DLL in the folder never costs the others. Returns how many
 * loaded; a folder that is not there is 0, not a failure.
 */
WBSH_SDK_API int wbshSdkLoadDirectory(const wchar_t* directory, WbshLogFn log, void* context);

/**
 * @brief Unloads everything, newest first. Hosts only.
 *
 * Anything a util registered points into its DLL, so the host must have
 * dropped every registration before this is called.
 */
WBSH_SDK_API void wbshSdkUnloadAll(void);

/** How many utils are loaded, and what they said about themselves. */
WBSH_SDK_API int wbshSdkCount(void);
WBSH_SDK_API const WbshUtilInfo* wbshSdkInfoAt(int index);

#ifdef __cplusplus
}  /* extern "C" */
#endif /* __cplusplus */
