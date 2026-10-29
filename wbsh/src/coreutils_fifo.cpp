/**
 * @file coreutils_fifo.cpp
 * @brief mkfifo and mknod: FIFOs as fifo.h makes them; Windows has no device nodes.
 */

#include "coreutils_internal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fifo.h"
#include "optscan.h"
#include "pathconv.h"

namespace wbsh {

	namespace fifo_detail {

		enum FifoOption { kFifoMode = 1, kFifoContext, kFifoHelp, kFifoVersion };

		struct FifoOptions {
			std::string mode;
			bool help = false;
			bool version = false;
			std::vector<std::string> operands;
		};

	}  // namespace fifo_detail

	using fifo_detail::FifoOptions;

	static const unsigned kOwnerWrite = 0200;

	static const char* const kMkfifoUsage =
		"Usage: mkfifo [OPTION]... NAME...\n"
		"Create named pipes (FIFOs) with the given NAMEs.\n"
		"\n"
		"  -m, --mode=MODE    set file permission bits to MODE\n"
		"  -Z, --context      accepted and ignored (no SELinux)\n"
		"      --help         display this help and exit\n"
		"      --version      output version information and exit\n"
		"\n"
		"A FIFO is a marker file that wbsh joins through a Windows named pipe:\n"
		"redirections and the bundled commands use it as a FIFO, other programs\n"
		"see the marker.\n";

	static const char* const kMknodUsage =
		"Usage: mknod [OPTION]... NAME TYPE [MAJOR MINOR]\n"
		"Create the special file NAME of the given TYPE.\n"
		"\n"
		"  -m, --mode=MODE    set file permission bits to MODE\n"
		"  -Z, --context      accepted and ignored (no SELinux)\n"
		"      --help         display this help and exit\n"
		"      --version      output version information and exit\n"
		"\n"
		"TYPE p makes a FIFO, as mkfifo does. Windows has no device nodes, so\n"
		"b, c and u are refused.\n";

	static int showText(const char* text) {
		std::fputs(text, stdout);
		std::fflush(stdout);
		return 0;
	}

	static int showVersion(const char* cmd) {
		std::printf("%s (wbsh) %d.%d.%d\n", cmd, WBSH_VERSION_MAJOR, WBSH_VERSION_MINOR,
			WBSH_VERSION_PATCH);
		std::fflush(stdout);
		return 0;
	}

	static std::string quoted(const std::string& text) {
		return "'" + text + "'";
	}

	static bool parseFifoArgs(const char* cmd, const std::vector<std::string>& args,
	                          FifoOptions& options) {
		static const std::vector<OptDef> defs = {
			{ fifo_detail::kFifoMode,    'm', "mode",    OptArg::Required },
			{ fifo_detail::kFifoContext, 'Z', "context", OptArg::Optional },
			{ fifo_detail::kFifoHelp,    0,   "help",    OptArg::None },
			{ fifo_detail::kFifoVersion, 0,   "version", OptArg::None },
		};

		OptScanResult scanned;
		if (!scanOptions(cmd, args, defs, scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (hit.id == fifo_detail::kFifoMode) options.mode = hit.value;
			if (hit.id == fifo_detail::kFifoHelp) options.help = true;
			if (hit.id == fifo_detail::kFifoVersion) options.version = true;
		}

		options.operands = scanned.operands;
		return true;
	}

	static bool readOnlyMode(const char* cmd, const std::string& text, bool& read_only) {
		read_only = false;
		if (text.empty()) return true;

		unsigned mode = 0;
		if (!parseFileMode(text, mode)) {
			perr(cmd, "invalid mode " + quoted(text));
			return false;
		}

		read_only = (mode & kOwnerWrite) == 0;
		return true;
	}

	static void markReadOnly(const std::filesystem::path& native) {
		const std::wstring path = longPathSafe(native).native();
		const DWORD attributes = ::GetFileAttributesW(path.c_str());
		if (attributes != INVALID_FILE_ATTRIBUTES) {
			::SetFileAttributesW(path.c_str(), attributes | FILE_ATTRIBUTE_READONLY);
		}
	}

	static bool makeFifo(Executor& exec, const char* cmd, const std::string& name, bool read_only) {
		const std::filesystem::path native = toNative(exec, name);
		std::error_code ec;
		if (!createFifoFile(native, ec)) {
			perr(cmd, "cannot create fifo " + quoted(name) + ": " + posixErrorText(ec));
			return false;
		}

		if (read_only) markReadOnly(native);
		return true;
	}

	static int builtin_mkfifo(Executor& exec, const std::vector<std::string>& args) {
		FifoOptions options;
		if (!parseFifoArgs("mkfifo", args, options)) return 1;
		if (options.help) return showText(kMkfifoUsage);
		if (options.version) return showVersion("mkfifo");
		if (options.operands.empty()) {
			perr("mkfifo", "missing operand");
			return 1;
		}

		bool read_only = false;
		if (!readOnlyMode("mkfifo", options.mode, read_only)) return 1;

		int status = 0;
		for (const std::string& name : options.operands) {
			if (!makeFifo(exec, "mkfifo", name, read_only)) status = 1;
		}

		return status;
	}

	static bool isDeviceType(const std::string& type) {
		return type == "b" || type == "c" || type == "u";
	}

	// GNU mknod's checks, in its order, before anything is made.
	static bool checkMknodOperands(const std::vector<std::string>& operands) {
		if (operands.size() < 2) {
			perr("mknod", operands.empty() ? "missing operand"
				: "missing operand after " + quoted(operands[0]));
			return false;
		}

		const std::string& type = operands[1];
		if (type != "p" && !isDeviceType(type)) {
			perr("mknod", "invalid device type " + quoted(type));
			return false;
		}

		if (type == "p" && operands.size() > 2) {
			perr("mknod", "Fifos do not have major and minor device numbers.");
			return false;
		}

		if (isDeviceType(type) && operands.size() != 4) {
			perr("mknod", "Special files require major and minor device numbers.");
			return false;
		}

		return true;
	}

	static int builtin_mknod(Executor& exec, const std::vector<std::string>& args) {
		FifoOptions options;
		if (!parseFifoArgs("mknod", args, options)) return 1;
		if (options.help) return showText(kMknodUsage);
		if (options.version) return showVersion("mknod");
		if (!checkMknodOperands(options.operands)) return 1;

		bool read_only = false;
		if (!readOnlyMode("mknod", options.mode, read_only)) return 1;

		const std::string& name = options.operands[0];
		if (options.operands[1] == "p") return makeFifo(exec, "mknod", name, read_only) ? 0 : 1;

		perr("mknod", quoted(name) + ": Operation not permitted");
		return 1;
	}

	void registerFifoBuiltins(Executor& exec) {
		exec.registerBuiltin("mkfifo", builtin_mkfifo);
		exec.registerBuiltin("mknod",  builtin_mknod);
	}

}  // namespace wbsh
