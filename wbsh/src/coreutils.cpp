/**
 * @file coreutils.cpp
 * @brief File / system coreutils plus the helpers shared by the
 *        coreutils_*.cpp family. Scope is pragmatic: enough flags to
 *        cover everyday interactive use and common scripts.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "awk.h"
#include "coreutils_internal.h"
#include "executor.h"
#include "inflate.h"
#include "numparse.h"
#include "regexutil.h"
#include "termreq.h"

namespace wbsh {

	namespace fs = std::filesystem;

	static const int         kFallbackConsoleColumns = 80;
	static const int         kFallbackConsoleRows    = 24;
	static const std::size_t kStreamChunk            = 4096;
	static const std::size_t kTimeTextCapacity       = 256;
	static const long        kDefaultLineCount       = 10;
	static const int         kDefaultUserId          = 1000;
	static const int         kDefaultGroupId         = 1000;
	static const double      kSeqEpsilon             = 1e-12;
	static const int         kMktempAttempts         = 200;
	static const std::size_t kMktempMinPlaceholders  = 3;
	static const int         kSignalTerm             = 15;

	void perr(const std::string& cmd, const std::string& path, const std::error_code& ec) {
		std::fprintf(stderr, "wbsh: %s: %s: %s\n",
			cmd.c_str(), path.c_str(), ec.message().c_str());
	}

	void perr(const std::string& cmd, const std::string& msg) {
		std::fprintf(stderr, "wbsh: %s: %s\n", cmd.c_str(), msg.c_str());
	}

	fs::path toNative(Executor& exec, const std::string& p) {
		return utf8ToPath(exec.pathConv().toWin32(p));
	}

	std::FILE* fopenNative(Executor& exec, const std::string& p, const char* mode) {
		return openUtf8(exec.pathConv().toWin32(p), mode);
	}

	static bool startsWithDash(const std::string& arg) {
		return !arg.empty() && arg[0] == '-';
	}

	static bool isOptionNotStdin(const std::string& arg) {
		return startsWithDash(arg) && arg != "-";
	}

	static bool isShortFlagCluster(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-' && arg[1] != '-';
	}

	static bool hasShortFlag(const std::string& cluster, char flag) {
		return cluster.find(flag, 1) != std::string::npos;
	}

	static bool isDigitChar(char c) {
		return std::isdigit(static_cast<unsigned char>(c)) != 0;
	}

	static std::string withErrno(const std::string& path) {
		return path + ": " + std::strerror(errno);
	}

	static std::FILE* openInputOrStdin(Executor& exec, const std::string& name) {
		if (name == "-") return stdin;
		return fopenNative(exec, name, "rb");
	}

	static void closeUnlessStdin(std::FILE* stream) {
		if (stream != stdin) std::fclose(stream);
	}

	static void takeRemainingOperands(const std::vector<std::string>& args, std::size_t from,
	                                  std::vector<std::string>& operands) {
		for (std::size_t i = from; i < args.size(); ++i) operands.push_back(args[i]);
	}

	static std::string joinWords(const std::vector<std::string>& words) {
		std::string joined;
		for (std::size_t i = 0; i < words.size(); ++i) {
			if (i != 0) joined.push_back(' ');
			joined += words[i];
		}

		return joined;
	}

	static std::string currentUserName(Executor& exec) {
		std::string user = exec.env().get("USER");
		if (user.empty()) user = exec.env().get("USERNAME");
		if (user.empty()) user = "user";
		return user;
	}

	static std::tm brokenDownTime(std::time_t epoch, bool utc) {
		std::tm result{};
#ifdef _WIN32
		if (utc) {
			gmtime_s(&result, &epoch);
			return result;
		}

		localtime_s(&result, &epoch);
#else
		if (utc) {
			gmtime_r(&epoch, &result);
			return result;
		}

		localtime_r(&epoch, &result);
#endif
		return result;
	}

	static std::string formatTime(const std::tm& time, const char* format) {
		char text[kTimeTextCapacity];
		std::strftime(text, sizeof(text), format, &time);
		return text;
	}

	static bool stdoutIsTty() {
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return false;
#endif
	}

	static bool queryConsoleSize(int& columns, int& rows) {
#ifdef _WIN32
		const HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (handle == INVALID_HANDLE_VALUE) return false;

		CONSOLE_SCREEN_BUFFER_INFO info{};
		if (::GetConsoleScreenBufferInfo(handle, &info) == 0) return false;

		columns = info.srWindow.Right - info.srWindow.Left + 1;
		rows    = info.srWindow.Bottom - info.srWindow.Top + 1;
		return true;
#else
		(void)columns;
		(void)rows;
		return false;
#endif
	}

	static int consoleWidth() {
		int columns = 0;
		int rows = 0;
		if (queryConsoleSize(columns, rows) && columns > 0) return columns;
		return kFallbackConsoleColumns;
	}

	struct LsOpts {
		bool all = false;
		bool long_fmt = false;
		bool one = false;
		bool human = false;
		bool reverse = false;
		bool sort_mtime = false;
		bool sort_size = false;
		bool classify = false;
		bool use_color = false;
		enum { Auto, Always, Never } color = Auto;
	};

	struct LsEntry {
		std::string name;
		fs::path full;
		fs::file_status status{};
		std::uintmax_t size = 0;
		fs::file_time_type mtime{};
		bool is_dir = false;
		bool is_symlink = false;
		bool is_executable = false;
		bool is_hidden = false;
		bool valid = false;
	};

	static bool windowsHidden(const fs::path& path) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		return (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#else
		(void)path;
		return false;
#endif
	}

	static bool windowsReadOnly(const fs::path& path) {
#ifdef _WIN32
		const DWORD attributes = ::GetFileAttributesW(path.wstring().c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) return false;
		return (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
		(void)path;
		return false;
#endif
	}

	static bool hasExecutableExtension(const fs::path& full) {
		std::string ext = pathToUtf8(full.extension());
		std::transform(ext.begin(), ext.end(), ext.begin(),
			[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		return ext == ".exe" || ext == ".bat" || ext == ".cmd" || ext == ".com" || ext == ".ps1";
	}

	static LsEntry collectEntry(const fs::path& parent, const std::string& name) {
		LsEntry entry;
		entry.name = name;
		const fs::path native = utf8ToPath(name);
		entry.full = parent.empty() ? native : (parent / native);

		std::error_code ec;
		entry.status = fs::symlink_status(entry.full, ec);
		entry.is_symlink = !ec && fs::is_symlink(entry.status);
		const fs::file_status real = entry.is_symlink ? fs::status(entry.full, ec) : entry.status;
		entry.is_dir = !ec && fs::is_directory(real);
		if (!ec && fs::is_regular_file(entry.status)) {
			entry.size = fs::file_size(entry.full, ec);
			if (ec) entry.size = 0;
		}

		std::error_code mtime_ec;
		entry.mtime = fs::last_write_time(entry.full, mtime_ec);
		entry.is_hidden = (!name.empty() && name[0] == '.') || windowsHidden(entry.full);
		entry.is_executable = hasExecutableExtension(entry.full);
		entry.valid = true;
		return entry;
	}

	static const char* lsColorCode(const LsEntry& entry) {
		if (entry.is_symlink) return "\x1b[36;1m";
		if (entry.is_dir) return "\x1b[34;1m";
		if (entry.is_executable) return "\x1b[32;1m";
		return nullptr;
	}

	static std::string colorize(const LsEntry& entry, bool use_color) {
		if (!use_color) return entry.name;

		const char* code = lsColorCode(entry);
		if (code == nullptr) return entry.name;
		return std::string(code) + entry.name + "\x1b[0m";
	}

	static std::string classifySuffix(const LsEntry& entry) {
		if (entry.is_dir) return "/";
		if (entry.is_symlink) return "@";
		if (entry.is_executable) return "*";
		return "";
	}

	static std::string humanSize(std::uintmax_t bytes) {
		static const char* const kUnits[] = { "", "K", "M", "G", "T", "P" };
		static const int kUnitCount = static_cast<int>(sizeof(kUnits) / sizeof(kUnits[0]));

		double value = static_cast<double>(bytes);
		int unit = 0;
		while (value >= 1024.0 && unit + 1 < kUnitCount) {
			value /= 1024.0;
			++unit;
		}

		char text[32];
		if (unit == 0) {
			std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(bytes));
		} else if (value >= 10.0) {
			std::snprintf(text, sizeof(text), "%.0f%s", value, kUnits[unit]);
		} else {
			std::snprintf(text, sizeof(text), "%.1f%s", value, kUnits[unit]);
		}

		return text;
	}

	static std::string formatMtime(const fs::file_time_type& mtime) {
		using namespace std::chrono;
		const auto system_time = time_point_cast<system_clock::duration>(
			mtime - fs::file_time_type::clock::now() + system_clock::now());
		const std::time_t epoch = system_clock::to_time_t(system_time);
		return formatTime(brokenDownTime(epoch, false), "%Y-%m-%d %H:%M");
	}

	static char permTypeChar(const LsEntry& entry) {
		if (entry.is_symlink) return 'l';
		if (entry.is_dir) return 'd';
		return '-';
	}

	static std::string permString(const LsEntry& entry) {
		const char* execute = (entry.is_executable || entry.is_dir) ? "x" : "-";
		const char* write = windowsReadOnly(entry.full) ? "-" : "w";

		std::string perms;
		perms += permTypeChar(entry);
		perms += "r";
		perms += write;
		perms += execute;
		perms += "r-";
		perms += execute;
		perms += "r-";
		perms += execute;
		return perms;
	}

	static bool lsNameLess(const LsEntry& a, const LsEntry& b) {
		const std::size_t common = std::min(a.name.size(), b.name.size());
		for (std::size_t i = 0; i < common; ++i) {
			const int ca = std::tolower(static_cast<unsigned char>(a.name[i]));
			const int cb = std::tolower(static_cast<unsigned char>(b.name[i]));
			if (ca != cb) return ca < cb;
		}

		if (a.name.size() != b.name.size()) return a.name.size() < b.name.size();
		return a.name < b.name;
	}

	static bool lsMtimeLess(const LsEntry& a, const LsEntry& b) {
		if (a.mtime != b.mtime) return a.mtime > b.mtime;
		return lsNameLess(a, b);
	}

	static bool lsSizeLess(const LsEntry& a, const LsEntry& b) {
		if (a.size != b.size) return a.size > b.size;
		return lsNameLess(a, b);
	}

	using LsLess = bool (*)(const LsEntry&, const LsEntry&);

	static LsLess lsComparator(const LsOpts& opts) {
		if (opts.sort_mtime) return lsMtimeLess;
		if (opts.sort_size) return lsSizeLess;
		return lsNameLess;
	}

	static void sortEntries(std::vector<LsEntry>& items, const LsOpts& opts) {
		std::sort(items.begin(), items.end(), lsComparator(opts));
		if (opts.reverse) std::reverse(items.begin(), items.end());
	}

	static std::vector<std::string> lsLabels(const std::vector<LsEntry>& items) {
		std::vector<std::string> labels;
		labels.reserve(items.size());
		for (const auto& entry : items) labels.push_back(entry.name + classifySuffix(entry));
		return labels;
	}

	static std::size_t longestLabel(const std::vector<std::string>& labels) {
		std::size_t longest = 0;
		for (const auto& label : labels) longest = std::max(longest, label.size());
		return longest;
	}

	static void printOnePerLine(const std::vector<LsEntry>& items, bool use_color) {
		for (const auto& entry : items) {
			std::fputs(colorize(entry, use_color).c_str(), stdout);
			std::fputs(classifySuffix(entry).c_str(), stdout);
			std::fputc('\n', stdout);
		}
	}

	static void printPadding(std::size_t from, std::size_t to) {
		for (std::size_t k = from; k < to; ++k) std::fputc(' ', stdout);
	}

	static void printGrid(const std::vector<LsEntry>& items, const std::vector<std::string>& labels,
	                      bool use_color) {
		const std::size_t pad  = longestLabel(labels) + 2;
		const std::size_t cols = std::max<std::size_t>(1, consoleWidth() / pad);
		const std::size_t rows = (items.size() + cols - 1) / cols;

		for (std::size_t row = 0; row < rows; ++row) {
			for (std::size_t col = 0; col < cols; ++col) {
				const std::size_t index = col * rows + row;
				if (index >= items.size()) break;

				const std::string shown = colorize(items[index], use_color)
					+ classifySuffix(items[index]);
				std::fputs(shown.c_str(), stdout);

				const bool more_in_row = col + 1 < cols && (col + 1) * rows + row < items.size();
				if (more_in_row) printPadding(labels[index].size(), pad);
			}

			std::fputc('\n', stdout);
		}
	}

	static void printColumns(const std::vector<LsEntry>& items, const LsOpts& opts) {
		if (items.empty()) return;
		if (opts.one || !stdoutIsTty()) {
			printOnePerLine(items, opts.use_color);
			return;
		}

		printGrid(items, lsLabels(items), opts.use_color);
	}

	static std::string lsSizeText(const LsEntry& entry, const LsOpts& opts) {
		if (opts.human) return humanSize(entry.size);
		return std::to_string(entry.size);
	}

	static std::size_t widestSizeText(const std::vector<LsEntry>& items, const LsOpts& opts) {
		std::size_t widest = 1;
		for (const auto& entry : items) widest = std::max(widest, lsSizeText(entry, opts).size());
		return widest;
	}

	static void printLong(const std::vector<LsEntry>& items, const LsOpts& opts, Executor& exec) {
		const std::string user = currentUserName(exec);
		const std::size_t size_width = widestSizeText(items, opts);

		for (const auto& entry : items) {
			const std::string size = lsSizeText(entry, opts);
			const std::string padding(size_width - size.size(), ' ');
			const std::string label = colorize(entry, opts.use_color) + classifySuffix(entry);
			std::fprintf(stdout, "%s 1 %s %s %s%s %s %s\n",
				permString(entry).c_str(), user.c_str(), user.c_str(),
				padding.c_str(), size.c_str(), formatMtime(entry.mtime).c_str(), label.c_str());
		}
	}

	static bool applyLsLongOption(const std::string& arg, LsOpts& opts) {
		if      (arg == "--color" || arg == "--color=auto")       opts.color = LsOpts::Auto;
		else if (arg == "--color=always" || arg == "--color=yes") opts.color = LsOpts::Always;
		else if (arg == "--color=never"  || arg == "--color=no")  opts.color = LsOpts::Never;
		else if (arg == "--all")                                  opts.all = true;
		else if (arg == "--human-readable")                       opts.human = true;
		else if (arg == "--reverse")                              opts.reverse = true;
		else if (arg == "--classify")                             opts.classify = true;
		else return false;
		return true;
	}

	static bool applyLsShortFlags(const std::string& cluster, LsOpts& opts) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			switch (cluster[k]) {
			case 'a': case 'A': opts.all = true; break;
			case 'l': opts.long_fmt = true; break;
			case '1': opts.one = true; break;
			case 'h': opts.human = true; break;
			case 'r': opts.reverse = true; break;
			case 't': opts.sort_mtime = true; break;
			case 'S': opts.sort_size = true; break;
			case 'F': opts.classify = true; break;
			default:
				std::fprintf(stderr, "wbsh: ls: unknown option -%c\n", cluster[k]);
				return false;
			}
		}

		return true;
	}

	static int parseLsArgs(const std::vector<std::string>& args,
	                       LsOpts& opts, std::vector<std::string>& paths) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				takeRemainingOperands(args, i + 1, paths);
				break;
			}

			if (applyLsLongOption(arg, opts)) continue;
			if (isShortFlagCluster(arg)) {
				if (!applyLsShortFlags(arg, opts)) return 2;
				continue;
			}

			paths.push_back(arg);
		}

		if (paths.empty()) paths.push_back(".");
		opts.use_color = opts.color == LsOpts::Always
			|| (opts.color == LsOpts::Auto && stdoutIsTty());
		return 0;
	}

	static bool collectLsDirectoryEntries(const fs::path& target, const LsOpts& opts,
	                                      const std::string& source_path,
	                                      std::vector<LsEntry>& items) {
		std::error_code ec;
		fs::directory_iterator it(target, ec);
		if (ec) {
			perr("ls", source_path, ec);
			return false;
		}

		for (const auto& dir_entry : it) {
			LsEntry entry = collectEntry(target, pathToUtf8(dir_entry.path().filename()));
			if (!opts.all && entry.is_hidden) continue;
			items.push_back(std::move(entry));
		}

		return true;
	}

	static void emitLsItems(const std::vector<LsEntry>& items, const LsOpts& opts, Executor& exec) {
		if (opts.long_fmt) printLong(items, opts, exec);
		else               printColumns(items, opts);
	}

	enum class LsHeader { None, First, Subsequent };

	static void printLsHeader(const std::string& path, LsHeader header) {
		if (header == LsHeader::None) return;
		if (header == LsHeader::Subsequent) std::fputc('\n', stdout);
		std::fprintf(stdout, "%s:\n", path.c_str());
	}

	static int listOnePath(Executor& exec, const LsOpts& opts, const std::string& path,
	                       LsHeader header) {
		const fs::path target = toNative(exec, path);
		std::error_code ec;
		const fs::file_status status = fs::symlink_status(target, ec);
		if (ec) {
			perr("ls", path, ec);
			return 1;
		}

		std::vector<LsEntry> items;
		if (fs::is_directory(status)) {
			printLsHeader(path, header);
			if (!collectLsDirectoryEntries(target, opts, path, items)) return 1;
		} else {
			LsEntry entry = collectEntry(fs::path(), path);
			entry.name = path;
			items.push_back(std::move(entry));
		}

		sortEntries(items, opts);
		emitLsItems(items, opts, exec);
		return 0;
	}

	static LsHeader lsHeaderFor(std::size_t index, std::size_t path_count) {
		if (path_count <= 1) return LsHeader::None;
		if (index == 0) return LsHeader::First;
		return LsHeader::Subsequent;
	}

	static int builtin_ls(Executor& exec, const std::vector<std::string>& args) {
		LsOpts opts;
		std::vector<std::string> paths;
		const int parse_status = parseLsArgs(args, opts, paths);
		if (parse_status != 0) return parse_status;

		int status = 0;
		for (std::size_t i = 0; i < paths.size(); ++i) {
			const LsHeader header = lsHeaderFor(i, paths.size());
			if (listOnePath(exec, opts, paths[i], header) != 0) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

	struct CatOptions {
		bool number = false;
		bool number_nonblank = false;
		std::vector<std::string> files;
	};

	struct CatEmitState {
		bool at_line_start = true;
		std::size_t lineno = 0;
	};

	// Flags seen before an unknown letter stay applied; the whole cluster
	// is then treated as a file name, as it always was.
	static bool applyCatShortFlags(const std::string& cluster, CatOptions& opts) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			if (cluster[k] == 'n') opts.number = true;
			else if (cluster[k] == 'b') opts.number_nonblank = true;
			else return false;
		}

		return true;
	}

	static CatOptions parseCatArgs(const std::vector<std::string>& args) {
		CatOptions opts;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				takeRemainingOperands(args, i + 1, opts.files);
				break;
			}

			if (arg == "-n" || arg == "--number") {
				opts.number = true;
				continue;
			}

			if (arg == "-b" || arg == "--number-nonblank") {
				opts.number_nonblank = true;
				continue;
			}

			if (isShortFlagCluster(arg) && applyCatShortFlags(arg, opts)) continue;
			opts.files.push_back(arg);
		}

		if (opts.files.empty()) opts.files.push_back("-");
		return opts;
	}

	static void catEmitChunk(const char* data, std::size_t length,
	                         const CatOptions& opts, CatEmitState& state) {
		if (!opts.number && !opts.number_nonblank) {
			std::fwrite(data, 1, length, stdout);
			return;
		}

		for (std::size_t i = 0; i < length; ++i) {
			if (state.at_line_start) {
				const bool blank_line = data[i] == '\n';
				if (!opts.number_nonblank || !blank_line) {
					++state.lineno;
					std::fprintf(stdout, "%6zu\t", state.lineno);
				}

				state.at_line_start = false;
			}

			std::fputc(data[i], stdout);
			if (data[i] == '\n') state.at_line_start = true;
		}
	}

	static void catStdin(const CatOptions& opts, CatEmitState& state) {
		char chunk[kStreamChunk];
		for (;;) {
			const std::size_t got = std::fread(chunk, 1, sizeof(chunk), stdin);
			if (got == 0) break;
			catEmitChunk(chunk, got, opts, state);
		}
	}

	static int catStreamFile(Executor& exec, const std::string& name,
	                         const CatOptions& opts, CatEmitState& state) {
		if (name == "-") {
			catStdin(opts, state);
			return 0;
		}

		std::ifstream in(toNative(exec, name), std::ios::binary);
		if (!in) {
			perr("cat", withErrno(name));
			return 1;
		}

		char chunk[kStreamChunk];
		while (in) {
			in.read(chunk, sizeof(chunk));
			const std::streamsize got = in.gcount();
			if (got > 0) catEmitChunk(chunk, static_cast<std::size_t>(got), opts, state);
		}

		return 0;
	}

	static int builtin_cat(Executor& exec, const std::vector<std::string>& args) {
		const CatOptions opts = parseCatArgs(args);
		CatEmitState state;
		int status = 0;
		for (const auto& file : opts.files) {
			if (catStreamFile(exec, file, opts, state) != 0) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

	static int builtin_clear(Executor&, const std::vector<std::string>&) {
		std::fputs("\x1b[H\x1b[2J\x1b[3J", stdout);
		std::fflush(stdout);
		requestScrollbackClear();
		return 0;
	}

	// A lone drive letter before ':' belongs to a Win32 entry, not to a
	// POSIX-style separator.
	static std::vector<std::string> splitSearchPath(const std::string& path) {
		std::vector<std::string> dirs;
		std::string current;
		for (char c : path) {
			const bool drive_colon = c == ':' && current.size() == 1
				&& std::isalpha(static_cast<unsigned char>(current[0])) != 0;
			if (c == ';' || (c == ':' && !drive_colon)) {
				dirs.push_back(current);
				current.clear();
				continue;
			}

			current.push_back(c);
		}

		if (!current.empty()) dirs.push_back(current);
		return dirs;
	}

	static bool findExecutableInDir(Executor& exec, const std::string& dir,
	                                const std::string& name, std::string& found) {
#ifdef _WIN32
		static const char* const kExtensions[] = { "", ".exe", ".cmd", ".bat", nullptr };
#else
		static const char* const kExtensions[] = { "", nullptr };
#endif
		const fs::path base = utf8ToPath(exec.pathConv().toWin32(dir));
		for (int e = 0; kExtensions[e] != nullptr; ++e) {
			const fs::path candidate = base / utf8ToPath(name + kExtensions[e]);
			std::error_code ec;
			if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
				found = exec.pathConv().toPosix(pathToUtf8(candidate));
				return true;
			}
		}

		return false;
	}

	static bool findOnSearchPath(Executor& exec, const std::string& name, std::string& found) {
		for (const auto& dir : splitSearchPath(exec.env().get("PATH"))) {
			if (dir.empty()) continue;
			if (findExecutableInDir(exec, dir, name, found)) return true;
		}

		return false;
	}

	static int builtin_which(Executor& exec, const std::vector<std::string>& args) {
		int status = 0;
		for (const auto& name : args) {
			if (exec.isFunction(name)) {
				std::printf("%s: shell function\n", name.c_str());
				continue;
			}

			if (exec.isBuiltin(name)) {
				std::printf("%s: shell builtin\n", name.c_str());
				continue;
			}

			std::string found;
			if (findOnSearchPath(exec, name, found)) {
				std::printf("%s\n", found.c_str());
				continue;
			}

			std::fprintf(stderr, "wbsh: which: %s: not found\n", name.c_str());
			status = 1;
		}

		return status;
	}

	static bool makeDirectory(Executor& exec, const std::string& path, bool parents) {
		std::error_code ec;
		const fs::path native = toNative(exec, path);
		const bool created = parents
			? fs::create_directories(native, ec)
			: fs::create_directory(native, ec);
		if (ec) {
			perr("mkdir", path, ec);
			return false;
		}

		if (!created && !parents) {
			perr("mkdir", path + ": already exists");
			return false;
		}

		return true;
	}

	static int builtin_mkdir(Executor& exec, const std::vector<std::string>& args) {
		bool parents = false;
		std::vector<std::string> paths;
		for (const auto& arg : args) {
			if (arg == "--parents") parents = true;
			else if (isShortFlagCluster(arg)) parents = parents || hasShortFlag(arg, 'p');
			else paths.push_back(arg);
		}

		if (paths.empty()) {
			perr("mkdir", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : paths) {
			if (!makeDirectory(exec, path, parents)) status = 1;
		}

		return status;
	}

	static bool removeDirectory(Executor& exec, const std::string& path) {
		std::error_code ec;
		if (fs::remove(toNative(exec, path), ec)) return true;

		if (ec) perr("rmdir", path, ec);
		else    perr("rmdir", path + ": failed");
		return false;
	}

	static int builtin_rmdir(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("rmdir", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : args) {
			if (!removeDirectory(exec, path)) status = 1;
		}

		return status;
	}

	struct RmOptions {
		bool recursive = false;
		bool force = false;
		std::vector<std::string> paths;
	};

	static RmOptions parseRmArgs(const std::vector<std::string>& args) {
		RmOptions opts;
		for (const auto& arg : args) {
			if (arg == "--recursive") {
				opts.recursive = true;
				continue;
			}

			if (arg == "--force") {
				opts.force = true;
				continue;
			}

			if (isShortFlagCluster(arg)) {
				if (hasShortFlag(arg, 'r') || hasShortFlag(arg, 'R')) opts.recursive = true;
				if (hasShortFlag(arg, 'f')) opts.force = true;
				continue;
			}

			opts.paths.push_back(arg);
		}

		return opts;
	}

	static bool removePath(Executor& exec, const std::string& path, const RmOptions& opts) {
		std::error_code ec;
		const fs::path native = toNative(exec, path);
		if (!fs::exists(native, ec)) {
			if (opts.force) return true;
			perr("rm", path + ": no such file or directory");
			return false;
		}

		if (opts.recursive) {
			fs::remove_all(native, ec);
			if (!ec || opts.force) return true;
			perr("rm", path, ec);
			return false;
		}

		if (fs::is_directory(native, ec)) {
			perr("rm", path + ": is a directory");
			return false;
		}

		if (fs::remove(native, ec) || opts.force) return true;
		perr("rm", path, ec);
		return false;
	}

	static int builtin_rm(Executor& exec, const std::vector<std::string>& args) {
		const RmOptions opts = parseRmArgs(args);
		if (opts.paths.empty()) {
			if (opts.force) return 0;
			perr("rm", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : opts.paths) {
			if (!removePath(exec, path, opts)) status = 1;
		}

		return status;
	}

	static fs::path destinationFor(const fs::path& src, const fs::path& dst, bool dst_is_dir) {
		if (dst_is_dir) return dst / src.filename();
		return dst;
	}

	static bool copyPath(Executor& exec, const std::string& source, const fs::path& target,
	                     bool recursive) {
		fs::copy_options options = fs::copy_options::overwrite_existing;
		if (recursive) options |= fs::copy_options::recursive | fs::copy_options::copy_symlinks;

		std::error_code ec;
		fs::copy(toNative(exec, source), target, options, ec);
		if (!ec) return true;

		perr("cp", source, ec);
		return false;
	}

	static int builtin_cp(Executor& exec, const std::vector<std::string>& args) {
		bool recursive = false;
		std::vector<std::string> paths;
		for (const auto& arg : args) {
			if (arg == "--recursive" || arg == "-a") {
				recursive = true;
				continue;
			}

			if (isShortFlagCluster(arg)) {
				if (hasShortFlag(arg, 'r') || hasShortFlag(arg, 'R')) recursive = true;
				continue;
			}

			paths.push_back(arg);
		}

		if (paths.size() < 2) {
			perr("cp", "missing source/destination");
			return 1;
		}

		const fs::path dst = toNative(exec, paths.back());
		std::error_code ec;
		const bool dst_is_dir = fs::is_directory(dst, ec);

		int status = 0;
		for (std::size_t i = 0; i + 1 < paths.size(); ++i) {
			const fs::path target = destinationFor(toNative(exec, paths[i]), dst, dst_is_dir);
			if (!copyPath(exec, paths[i], target, recursive)) status = 1;
		}

		return status;
	}

	// A rename that fails (typically across volumes) falls back to a
	// copy followed by removal of the source.
	static bool movePath(const std::string& source, const fs::path& src, const fs::path& target) {
		std::error_code rename_ec;
		fs::rename(src, target, rename_ec);
		if (!rename_ec) return true;

		std::error_code copy_ec;
		fs::copy(src, target, fs::copy_options::overwrite_existing
			| fs::copy_options::recursive | fs::copy_options::copy_symlinks, copy_ec);
		if (copy_ec) {
			perr("mv", source, copy_ec);
			return false;
		}

		std::error_code remove_ec;
		fs::remove_all(src, remove_ec);
		if (!remove_ec) return true;

		perr("mv", source, remove_ec);
		return false;
	}

	static int builtin_mv(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> paths;
		for (const auto& arg : args) {
			if (arg == "--" || isShortFlagCluster(arg)) continue;
			paths.push_back(arg);
		}

		if (paths.size() < 2) {
			perr("mv", "missing source/destination");
			return 1;
		}

		const fs::path dst = toNative(exec, paths.back());
		std::error_code ec;
		const bool dst_is_dir = fs::is_directory(dst, ec);

		int status = 0;
		for (std::size_t i = 0; i + 1 < paths.size(); ++i) {
			const fs::path src = toNative(exec, paths[i]);
			if (!movePath(paths[i], src, destinationFor(src, dst, dst_is_dir))) status = 1;
		}

		return status;
	}

	static bool touchPath(Executor& exec, const std::string& path) {
		const fs::path native = toNative(exec, path);
		std::error_code ec;
		if (!fs::exists(native, ec)) {
			std::ofstream created(native, std::ios::binary | std::ios::app);
			if (created) return true;
			perr("touch", path + ": cannot create");
			return false;
		}

		fs::last_write_time(native, fs::file_time_type::clock::now(), ec);
		if (!ec) return true;

		perr("touch", path, ec);
		return false;
	}

	static int builtin_touch(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("touch", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : args) {
			if (isOptionNotStdin(path)) continue;
			if (!touchPath(exec, path)) status = 1;
		}

		return status;
	}

	// Returns 0 when a value was consumed (`i` advanced past it), 1 when
	// args[i] is not this flag at all, -1 when the value is malformed.
	static int parseNumFlag(const std::vector<std::string>& args, const char* short_flag,
	                        std::size_t& i, long& value) {
		const std::string& arg = args[i];
		const bool attached = arg.size() > 2 && arg[0] == '-' && arg[1] == short_flag[0]
			&& isDigitChar(arg[2]);
		if (attached) {
			int parsed = 0;
			if (!parseInt(arg.substr(2), parsed)) return -1;
			value = parsed;
			++i;
			return 0;
		}

		if (arg == short_flag || arg == std::string("-") + short_flag) {
			if (i + 1 >= args.size()) return -1;
			int parsed = 0;
			if (!parseInt(args[i + 1], parsed)) return -1;
			value = parsed;
			i += 2;
			return 0;
		}

		return 1;
	}

	static bool parseLegacyLineCount(const char* cmd, const std::string& arg, long& count) {
		int parsed = 0;
		if (!parseInt(arg.substr(1), parsed)) {
			perr(cmd, "bad N");
			return false;
		}

		count = parsed;
		return true;
	}

	static int parseLineCountArgs(const char* cmd, const std::vector<std::string>& args,
	                              long& count, std::vector<std::string>& files) {
		for (std::size_t i = 0; i < args.size(); ) {
			const std::string& arg = args[i];
			if (arg.rfind("-n", 0) == 0) {
				std::size_t next = i;
				const int parsed = parseNumFlag(args, "n", next, count);
				if (parsed == 0) {
					i = next;
					continue;
				}

				if (parsed == -1) {
					perr(cmd, "bad -n value");
					return 1;
				}
			}

			if (arg == "--") {
				takeRemainingOperands(args, i + 1, files);
				break;
			}

			if (arg.size() > 1 && arg[0] == '-' && isDigitChar(arg[1])) {
				if (!parseLegacyLineCount(cmd, arg, count)) return 1;
				++i;
				continue;
			}

			files.push_back(arg);
			++i;
		}

		if (files.empty()) files.push_back("-");
		return 0;
	}

	static void printFirstLines(std::FILE* stream, long count) {
		long printed = 0;
		int c = EOF;
		while (printed < count && (c = std::fgetc(stream)) != EOF) {
			std::fputc(c, stdout);
			if (c == '\n') ++printed;
		}
	}

	static int builtin_head(Executor& exec, const std::vector<std::string>& args) {
		long count = kDefaultLineCount;
		std::vector<std::string> files;
		if (parseLineCountArgs("head", args, count, files) != 0) return 1;

		int status = 0;
		for (const auto& file : files) {
			std::FILE* stream = openInputOrStdin(exec, file);
			if (stream == nullptr) {
				perr("head", withErrno(file));
				status = 1;
				continue;
			}

			printFirstLines(stream, count);
			closeUnlessStdin(stream);
		}

		std::fflush(stdout);
		return status;
	}

	static void printLastLines(const std::vector<std::string>& lines, long count) {
		const std::size_t wanted = static_cast<std::size_t>(count);
		const std::size_t start = lines.size() > wanted ? lines.size() - wanted : 0;
		for (std::size_t k = start; k < lines.size(); ++k) {
			std::fputs(lines[k].c_str(), stdout);
			std::fputc('\n', stdout);
		}
	}

	static int builtin_tail(Executor& exec, const std::vector<std::string>& args) {
		long count = kDefaultLineCount;
		std::vector<std::string> files;
		if (parseLineCountArgs("tail", args, count, files) != 0) return 1;

		int status = 0;
		for (const auto& file : files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, file, lines)) {
				perr("tail", withErrno(file));
				status = 1;
				continue;
			}

			printLastLines(lines, count);
		}

		std::fflush(stdout);
		return status;
	}

	static void appendWcCount(std::string& out, std::uintmax_t value) {
		if (!out.empty()) out += " ";
		char text[64];
		std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(value));
		out += text;
	}

	struct WcOptions {
		bool want_lines = false;
		bool want_words = false;
		bool want_bytes = false;
		bool want_chars = false;
		std::vector<std::string> files;
	};

	struct WcCounts {
		std::uintmax_t lines = 0;
		std::uintmax_t words = 0;
		std::uintmax_t bytes = 0;
		std::uintmax_t chars = 0;
	};

	static bool applyWcFlag(char flag, WcOptions& opts) {
		switch (flag) {
		case 'l': opts.want_lines = true; return true;
		case 'w': opts.want_words = true; return true;
		case 'c': opts.want_bytes = true; return true;
		case 'm': opts.want_chars = true; return true;
		default:  return false;
		}
	}

	static bool applyWcLongOption(const std::string& arg, WcOptions& opts) {
		if (arg == "--lines") return applyWcFlag('l', opts);
		if (arg == "--words") return applyWcFlag('w', opts);
		if (arg == "--bytes") return applyWcFlag('c', opts);
		if (arg == "--chars") return applyWcFlag('m', opts);
		return false;
	}

	static WcOptions parseWcArgs(const std::vector<std::string>& args) {
		WcOptions opts;
		for (const auto& arg : args) {
			if (arg == "--") continue;
			if (applyWcLongOption(arg, opts)) continue;
			if (isShortFlagCluster(arg)) {
				for (std::size_t k = 1; k < arg.size(); ++k) applyWcFlag(arg[k], opts);
				continue;
			}

			opts.files.push_back(arg);
		}

		if (!(opts.want_lines || opts.want_words || opts.want_bytes || opts.want_chars)) {
			opts.want_lines = true;
			opts.want_words = true;
			opts.want_bytes = true;
		}

		if (opts.files.empty()) opts.files.push_back("-");
		return opts;
	}

	static bool isUtf8LeadByte(int ch) {
		const unsigned char byte = static_cast<unsigned char>(ch);
		return byte < 0x80 || (byte & 0xC0) != 0x80;
	}

	static WcCounts wcCountStream(std::FILE* stream) {
		WcCounts counts;
		bool in_word = false;
		int ch = EOF;
		while ((ch = std::fgetc(stream)) != EOF) {
			++counts.bytes;
			if (isUtf8LeadByte(ch)) ++counts.chars;
			if (ch == '\n') ++counts.lines;

			if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
				in_word = false;
			} else if (!in_word) {
				in_word = true;
				++counts.words;
			}
		}

		return counts;
	}

	static void addWcCounts(WcCounts& totals, const WcCounts& counts) {
		totals.lines += counts.lines;
		totals.words += counts.words;
		totals.bytes += counts.bytes;
		totals.chars += counts.chars;
	}

	static std::string formatWcLine(const WcOptions& opts, const WcCounts& counts,
	                                const std::string& label) {
		std::string out;
		if (opts.want_lines) appendWcCount(out, counts.lines);
		if (opts.want_words) appendWcCount(out, counts.words);
		if (opts.want_chars && !opts.want_bytes) appendWcCount(out, counts.chars);
		if (opts.want_bytes) appendWcCount(out, counts.bytes);
		if (!label.empty()) {
			out += " ";
			out += label;
		}

		out.push_back('\n');
		return out;
	}

	static int builtin_wc(Executor& exec, const std::vector<std::string>& args) {
		const WcOptions opts = parseWcArgs(args);
		WcCounts totals;
		int status = 0;
		for (const auto& file : opts.files) {
			std::FILE* stream = openInputOrStdin(exec, file);
			if (stream == nullptr) {
				perr("wc", withErrno(file));
				status = 1;
				continue;
			}

			const WcCounts counts = wcCountStream(stream);
			closeUnlessStdin(stream);
			std::fputs(formatWcLine(opts, counts, file == "-" ? "" : file).c_str(), stdout);
			addWcCounts(totals, counts);
		}

		if (opts.files.size() > 1) {
			std::fputs(formatWcLine(opts, totals, "total").c_str(), stdout);
		}

		std::fflush(stdout);
		return status;
	}

	static int builtin_whoami(Executor& exec, const std::vector<std::string>&) {
		std::printf("%s\n", currentUserName(exec).c_str());
		return 0;
	}

	static std::string windowsComputerName() {
#ifdef _WIN32
		char name[256];
		DWORD length = sizeof(name);
		if (::GetComputerNameA(name, &length) != 0) return std::string(name, length);
#endif
		return {};
	}

	static int builtin_hostname(Executor& exec, const std::vector<std::string>&) {
		std::string host = exec.env().get("HOSTNAME");
		if (host.empty()) host = exec.env().get("COMPUTERNAME");
		if (host.empty()) host = windowsComputerName();
		std::printf("%s\n", host.c_str());
		return 0;
	}

	using EnvPair = std::pair<std::string, std::string>;

	static bool isEnvAssignment(const std::string& arg) {
		if (arg.find('=') == std::string::npos) return false;
		return std::isalpha(static_cast<unsigned char>(arg[0])) != 0 || arg[0] == '_';
	}

	static void applyEnvOverride(std::vector<EnvPair>& all, const EnvPair& assignment) {
		for (auto& entry : all) {
			if (entry.first != assignment.first) continue;
			entry.second = assignment.second;
			return;
		}

		all.push_back(assignment);
	}

	static int builtin_env(Executor& exec, const std::vector<std::string>& args) {
		std::vector<EnvPair> overrides;
		std::vector<std::string> command;
		for (const auto& arg : args) {
			if (command.empty() && isEnvAssignment(arg)) {
				const std::size_t eq = arg.find('=');
				overrides.emplace_back(arg.substr(0, eq), arg.substr(eq + 1));
			} else {
				command.push_back(arg);
			}
		}

		if (!command.empty()) {
			std::fprintf(stderr, "wbsh: env: running with overrides not yet implemented; "
				"set then call directly\n");
			return 1;
		}

		std::vector<EnvPair> all(exec.env().vars().begin(), exec.env().vars().end());
		for (const auto& assignment : overrides) applyEnvOverride(all, assignment);
		std::sort(all.begin(), all.end());

		for (const auto& entry : all) {
			if (!exec.env().isExported(entry.first)) continue;
			std::printf("%s=%s\n", entry.first.c_str(), entry.second.c_str());
		}

		return 0;
	}

	static bool parseSleepSeconds(const std::string& arg, double& seconds) {
		const char suffix = arg.empty() ? '\0' : arg.back();
		const bool has_unit = suffix == 's' || suffix == 'm' || suffix == 'h';
		const std::string number = has_unit ? arg.substr(0, arg.size() - 1) : arg;
		if (!parseDouble(number, seconds)) return false;

		if (suffix == 'm') seconds *= 60.0;
		else if (suffix == 'h') seconds *= 3600.0;
		return true;
	}

	static int builtin_sleep(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("sleep", "missing operand");
			return 1;
		}

		double seconds = 0.0;
		if (!parseSleepSeconds(args[0], seconds)) {
			perr("sleep", args[0] + ": invalid time interval");
			return 1;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(
			static_cast<long long>(seconds * 1000.0)));
		return 0;
	}

	static std::string stripTrailingSlashes(std::string path) {
		while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
		return path;
	}

	static void stripSuffix(std::string& name, const std::string& suffix) {
		if (name.size() <= suffix.size()) return;
		if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return;
		name.resize(name.size() - suffix.size());
	}

	static int builtin_basename(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("basename", "missing operand");
			return 1;
		}

		const std::string path = stripTrailingSlashes(args[0]);
		const std::size_t slash = path.find_last_of("/\\");
		std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
		if (args.size() > 1) stripSuffix(name, args[1]);

		std::printf("%s\n", name.c_str());
		return 0;
	}

	static int builtin_dirname(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("dirname", "missing operand");
			return 1;
		}

		const std::string path = stripTrailingSlashes(args[0]);
		const std::size_t slash = path.find_last_of("/\\");
		if (slash == std::string::npos) {
			std::printf(".\n");
			return 0;
		}

		if (slash == 0) {
			std::printf("/\n");
			return 0;
		}

		std::printf("%s\n", path.substr(0, slash).c_str());
		return 0;
	}

	static void appendLinesFromStream(std::FILE* stream, std::vector<std::string>& out) {
		std::string current;
		int c = EOF;
		while ((c = std::fgetc(stream)) != EOF) {
			if (c != '\n') {
				current.push_back(static_cast<char>(c));
				continue;
			}

			out.push_back(std::move(current));
			current.clear();
		}

		if (!current.empty()) out.push_back(std::move(current));
	}

	bool readAllLines(Executor& exec, const std::string& path,
	                  std::vector<std::string>& out) {
		std::FILE* stream = openInputOrStdin(exec, path);
		if (stream == nullptr) return false;

		appendLinesFromStream(stream, out);
		closeUnlessStdin(stream);
		return true;
	}

	// MSVC's strftime doesn't understand the glibc `%s` (epoch seconds)
	// extension: on Windows it hands the format to the CRT's invalid-
	// parameter handler, which aborts the whole process. Substitute it
	// ourselves before the real strftime call ever sees it.
	static std::string substituteDateEpochSpecifier(const std::string& fmt, std::time_t epoch) {
		std::string out;
		for (std::size_t i = 0; i < fmt.size(); ++i) {
			if (fmt[i] == '%' && i + 1 < fmt.size() && fmt[i + 1] == 's') {
				out += std::to_string(static_cast<long long>(epoch));
				++i;
				continue;
			}

			out += fmt[i];
		}

		return out;
	}

	static int builtin_date(Executor&, const std::vector<std::string>& args) {
		std::string fmt = "%a %b %e %H:%M:%S %Y";
		bool utc = false;
		for (const auto& arg : args) {
			if (arg == "-u" || arg == "--utc") utc = true;
			else if (!arg.empty() && arg[0] == '+') fmt = arg.substr(1);
		}

		const std::time_t now = std::time(nullptr);
		fmt = substituteDateEpochSpecifier(fmt, now);
		std::printf("%s\n", formatTime(brokenDownTime(now, utc), fmt.c_str()).c_str());
		return 0;
	}

	struct SeqRange {
		double first = 1.0;
		double inc = 1.0;
		double last = 1.0;
	};

	static bool parseSeqNumbers(const std::vector<std::string>& nums, SeqRange& range) {
		if (nums.size() == 1) return parseDouble(nums[0], range.last);
		if (nums.size() == 2) {
			return parseDouble(nums[0], range.first) && parseDouble(nums[1], range.last);
		}

		return parseDouble(nums[0], range.first) && parseDouble(nums[1], range.inc)
			&& parseDouble(nums[2], range.last);
	}

	static bool seqContinues(double value, const SeqRange& range) {
		if (range.inc > 0) return value <= range.last + kSeqEpsilon;
		return value >= range.last - kSeqEpsilon;
	}

	static bool seqIsIntegral(const SeqRange& range) {
		return std::floor(range.first) == range.first && std::floor(range.inc) == range.inc
			&& std::floor(range.last) == range.last;
	}

	static void printSeq(const SeqRange& range, const std::string& sep) {
		const bool integral = seqIsIntegral(range);
		bool first_out = true;
		for (double value = range.first; seqContinues(value, range); value += range.inc) {
			if (!first_out) std::fputs(sep.c_str(), stdout);
			if (integral) std::fprintf(stdout, "%lld", static_cast<long long>(value));
			else          std::fprintf(stdout, "%g", value);
			first_out = false;
		}
	}

	static int builtin_seq(Executor&, const std::vector<std::string>& args) {
		std::string sep = "\n";
		std::vector<std::string> nums;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "--") {
				takeRemainingOperands(args, i + 1, nums);
				break;
			}

			if (arg == "-s" && i + 1 < args.size()) sep = args[++i];
			else if (arg.size() > 2 && arg.compare(0, 2, "-s") == 0) sep = arg.substr(2);
			else nums.push_back(arg);
		}

		if (nums.empty() || nums.size() > 3) {
			perr("seq", "usage: seq [LAST | FIRST LAST | FIRST INC LAST]");
			return 1;
		}

		SeqRange range;
		if (!parseSeqNumbers(nums, range)) {
			perr("seq", "invalid number");
			return 1;
		}

		if (range.inc == 0) {
			perr("seq", "increment must be non-zero");
			return 1;
		}

		printSeq(range, sep);
		std::fputc('\n', stdout);
		std::fflush(stdout);
		return 0;
	}

	struct UnameOptions {
		bool all = false;
		bool kernel = false;
		bool node = false;
		bool release = false;
		bool version = false;
		bool machine = false;
		bool opsys = false;
	};

	struct UnameInfo {
		std::string node;
		std::string arch;
		std::string release;
		std::string version;
	};

	static void applyUnameFlag(char flag, UnameOptions& opts) {
		switch (flag) {
		case 'a': opts.all = true; break;
		case 's': opts.kernel = true; break;
		case 'n': opts.node = true; break;
		case 'r': opts.release = true; break;
		case 'v': opts.version = true; break;
		case 'm': opts.machine = true; break;
		case 'o': opts.opsys = true; break;
		}
	}

	static bool applyUnameLongOption(const std::string& arg, UnameOptions& opts) {
		if      (arg == "--all")              applyUnameFlag('a', opts);
		else if (arg == "--kernel-name")      applyUnameFlag('s', opts);
		else if (arg == "--nodename")         applyUnameFlag('n', opts);
		else if (arg == "--kernel-release")   applyUnameFlag('r', opts);
		else if (arg == "--kernel-version")   applyUnameFlag('v', opts);
		else if (arg == "--machine")          applyUnameFlag('m', opts);
		else if (arg == "--operating-system") applyUnameFlag('o', opts);
		else return false;
		return true;
	}

	static UnameOptions parseUnameArgs(const std::vector<std::string>& args) {
		UnameOptions opts;
		for (const auto& arg : args) {
			if (applyUnameLongOption(arg, opts)) continue;
			if (!isShortFlagCluster(arg)) continue;
			for (std::size_t k = 1; k < arg.size(); ++k) applyUnameFlag(arg[k], opts);
		}

		const bool any = opts.all || opts.kernel || opts.node || opts.release
			|| opts.version || opts.machine || opts.opsys;
		if (!any) opts.kernel = true;
		if (opts.all) {
			opts.kernel = true;
			opts.node = true;
			opts.release = true;
			opts.version = true;
			opts.machine = true;
			opts.opsys = true;
		}

		return opts;
	}

	static UnameInfo collectUnameInfo(Executor& exec) {
		UnameInfo info;
		info.node = exec.env().get("COMPUTERNAME");
		if (info.node.empty()) info.node = exec.env().get("HOSTNAME");
#ifdef _WIN32
		info.arch = exec.env().get("PROCESSOR_ARCHITECTURE");
		if (info.arch.empty()) info.arch = "x86_64";

		OSVERSIONINFOA os{};
		os.dwOSVersionInfoSize = sizeof(os);
#  pragma warning(push)
#  pragma warning(disable : 4996)
		::GetVersionExA(&os);
#  pragma warning(pop)
		info.release = std::to_string(os.dwMajorVersion) + "." + std::to_string(os.dwMinorVersion);
		info.version = std::to_string(os.dwBuildNumber);
#else
		info.arch = "x86_64";
		info.release = "0";
		info.version = "0";
#endif
		return info;
	}

	static void appendWord(std::string& out, const std::string& word) {
		if (!out.empty()) out += " ";
		out += word;
	}

	static int builtin_uname(Executor& exec, const std::vector<std::string>& args) {
		const UnameOptions opts = parseUnameArgs(args);
		const UnameInfo info = collectUnameInfo(exec);

		std::string out;
		if (opts.kernel)  appendWord(out, "wbsh");
		if (opts.node)    appendWord(out, info.node);
		if (opts.release) appendWord(out, info.release);
		if (opts.version) appendWord(out, info.version);
		if (opts.machine) appendWord(out, info.arch);
		if (opts.opsys)   appendWord(out, "Windows");
		std::printf("%s\n", out.c_str());
		return 0;
	}

	static void printIdField(bool as_name, const std::string& user, int id) {
		if (as_name) std::printf("%s\n", user.c_str());
		else         std::printf("%d\n", id);
	}

	static int builtin_id(Executor& exec, const std::vector<std::string>& args) {
		bool name_only = false;
		bool user_only = false;
		bool group_only = false;
		for (const auto& arg : args) {
			if (arg == "-n") name_only = true;
			else if (arg == "-u") user_only = true;
			else if (arg == "-g") group_only = true;
		}

		const std::string user = currentUserName(exec);
		if (user_only) {
			printIdField(name_only, user, kDefaultUserId);
			return 0;
		}

		if (group_only) {
			printIdField(name_only, user, kDefaultGroupId);
			return 0;
		}

		std::printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)\n",
			kDefaultUserId, user.c_str(), kDefaultGroupId, user.c_str(),
			kDefaultGroupId, user.c_str());
		return 0;
	}

	static int builtin_realpath(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("realpath", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : args) {
			if (startsWithDash(path)) continue;

			std::error_code ec;
			const fs::path resolved = fs::weakly_canonical(toNative(exec, path), ec);
			if (ec) {
				perr("realpath", path, ec);
				status = 1;
				continue;
			}

			std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(resolved)).c_str());
		}

		return status;
	}

	static bool readLinkTarget(Executor& exec, const std::string& path, bool canonical,
	                           std::string& target) {
		std::error_code ec;
		const fs::path native = toNative(exec, path);
		const fs::path resolved = canonical
			? fs::weakly_canonical(native, ec)
			: fs::read_symlink(native, ec);
		if (ec) return false;

		target = exec.pathConv().toPosix(pathToUtf8(resolved));
		return true;
	}

	static int builtin_readlink(Executor& exec, const std::vector<std::string>& args) {
		bool canonical = false;
		std::vector<std::string> paths;
		for (const auto& arg : args) {
			if (arg == "-f" || arg == "-e" || arg == "-m" || arg == "--canonicalize") {
				canonical = true;
			} else if (!arg.empty() && arg[0] != '-') {
				paths.push_back(arg);
			}
		}

		if (paths.empty()) {
			perr("readlink", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : paths) {
			std::string target;
			if (!readLinkTarget(exec, path, canonical, target)) {
				status = 1;
				continue;
			}

			std::printf("%s\n", target.c_str());
		}

		return status;
	}

	static int exprPrintNumber(long long value) {
		std::printf("%lld\n", value);
		return 0;
	}

	static int exprPrintTruth(bool truth) {
		std::printf("%d\n", truth ? 1 : 0);
		return truth ? 0 : 1;
	}

	static int exprBinaryIntOp(long long left, long long right, const std::string& op) {
		if (op == "+") return exprPrintNumber(left + right);
		if (op == "-") return exprPrintNumber(left - right);
		if (op == "*") return exprPrintNumber(left * right);
		if (op == "/") {
			if (right == 0) return 2;
			return exprPrintNumber(left / right);
		}

		if (op == "%") {
			if (right == 0) return 2;
			return exprPrintNumber(left % right);
		}

		if (op == "<")  return exprPrintTruth(left <  right);
		if (op == "<=") return exprPrintTruth(left <= right);
		if (op == ">")  return exprPrintTruth(left >  right);
		if (op == ">=") return exprPrintTruth(left >= right);
		if (op == "=" || op == "==") return exprPrintTruth(left == right);
		if (op == "!=") return exprPrintTruth(left != right);
		return -1;
	}

	static int exprBinaryStringOp(const std::string& left, const std::string& right,
	                              const std::string& op) {
		if (op == "=" || op == "==") return exprPrintTruth(left == right);
		if (op == "!=") return exprPrintTruth(left != right);
		return -1;
	}

	static int exprSubstr(const std::vector<std::string>& args) {
		int position = 0;
		int length = 0;
		if (!parseInt(args[2], position) || !parseInt(args[3], length)) return 2;
		if (position < 1) position = 1;

		const std::size_t start = static_cast<std::size_t>(position - 1);
		if (start >= args[1].size()) {
			std::printf("\n");
			return 1;
		}

		std::printf("%s\n", args[1].substr(start, length).c_str());
		return 0;
	}

	// Returns -1 when the operator is not one expr knows, so the caller
	// falls back to echoing the operands.
	static int exprBinary(const std::vector<std::string>& args) {
		const std::string& left  = args[0];
		const std::string& op    = args[1];
		const std::string& right = args[2];

		long long left_int = 0;
		long long right_int = 0;
		if (parseLL(left, left_int) && parseLL(right, right_int)) {
			return exprBinaryIntOp(left_int, right_int, op);
		}

		return exprBinaryStringOp(left, right, op);
	}

	static int builtin_expr(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("expr", "missing operand");
			return 2;
		}

		if (args.size() == 2 && args[0] == "length") {
			std::printf("%zu\n", args[1].size());
			return 0;
		}

		if (args.size() == 4 && args[0] == "substr") return exprSubstr(args);
		if (args.size() == 3) {
			const int status = exprBinary(args);
			if (status >= 0) return status;
		}

		std::printf("%s\n", joinWords(args).c_str());
		return 0;
	}

	struct CmpOptions {
		bool quiet = false;
		std::vector<std::string> files;
	};

	static CmpOptions parseCmpArgs(const std::vector<std::string>& args) {
		CmpOptions opts;
		for (const auto& arg : args) {
			if (arg == "-s" || arg == "--silent" || arg == "--quiet") {
				opts.quiet = true;
				continue;
			}

			if (startsWithDash(arg)) continue;
			opts.files.push_back(arg);
		}

		return opts;
	}

	static int compareStreams(std::FILE* first, std::FILE* second, const CmpOptions& opts) {
		long long byte = 0;
		long long line = 1;
		for (;;) {
			const int a = std::fgetc(first);
			const int b = std::fgetc(second);
			if (a == EOF && b == EOF) return 0;

			++byte;
			if (a == EOF || b == EOF) {
				const std::string& ended = a == EOF ? opts.files[0] : opts.files[1];
				if (!opts.quiet) std::fprintf(stderr, "cmp: EOF on %s\n", ended.c_str());
				return 1;
			}

			if (a != b) {
				if (!opts.quiet) {
					std::printf("%s %s differ: byte %lld, line %lld\n",
						opts.files[0].c_str(), opts.files[1].c_str(), byte, line);
				}

				return 1;
			}

			if (a == '\n') ++line;
		}
	}

	static int builtin_cmp(Executor& exec, const std::vector<std::string>& args) {
		const CmpOptions opts = parseCmpArgs(args);
		if (opts.files.size() < 2) {
			perr("cmp", "usage: cmp [-s] FILE1 FILE2");
			return 2;
		}

		std::FILE* first = fopenNative(exec, opts.files[0], "rb");
		if (first == nullptr) {
			if (!opts.quiet) perr("cmp", withErrno(opts.files[0]));
			return 2;
		}

		std::FILE* second = fopenNative(exec, opts.files[1], "rb");
		if (second == nullptr) {
			std::fclose(first);
			if (!opts.quiet) perr("cmp", withErrno(opts.files[1]));
			return 2;
		}

		const int status = compareStreams(first, second, opts);
		std::fclose(first);
		std::fclose(second);
		return status;
	}

	static void diffEmitRange(int a, int b) {
		if (a == b) std::printf("%d", a);
		else        std::printf("%d,%d", a, b);
	}

	namespace diff_internal {
		struct DiffOptions {
			bool brief = false;
			bool unified = false;
			int context = 3;
			std::vector<std::string> files;
		};

		struct UnifiedItem {
			char kind;
			int a_line;
			int b_line;
			std::string text;
		};

		struct NormalHunk {
			int a1;
			int a2;
			int b1;
			int b2;
			char kind;
		};

		using Lines = std::vector<std::string>;
		using LcsTable = std::vector<std::vector<int>>;
	}  // namespace diff_internal

	static diff_internal::DiffOptions parseDiffArgs(const std::vector<std::string>& args) {
		diff_internal::DiffOptions opts;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-q" || arg == "--brief") {
				opts.brief = true;
				continue;
			}

			if (arg == "-u" || arg == "--unified") {
				opts.unified = true;
				continue;
			}

			if (arg.size() > 2 && arg.compare(0, 2, "-U") == 0) {
				opts.unified = true;
				parseInt(arg.substr(2), opts.context);
				continue;
			}

			if (arg == "-U" && i + 1 < args.size()) {
				opts.unified = true;
				parseInt(args[++i], opts.context);
				continue;
			}

			if (startsWithDash(arg)) continue;
			opts.files.push_back(arg);
		}

		return opts;
	}

	static diff_internal::LcsTable
	buildLcsTable(const diff_internal::Lines& left, const diff_internal::Lines& right) {
		const std::size_t m = left.size();
		const std::size_t n = right.size();
		diff_internal::LcsTable table(m + 1, std::vector<int>(n + 1, 0));
		for (std::size_t i = 1; i <= m; ++i) {
			for (std::size_t j = 1; j <= n; ++j) {
				if (left[i - 1] == right[j - 1]) {
					table[i][j] = table[i - 1][j - 1] + 1;
				} else {
					table[i][j] = (std::max)(table[i - 1][j], table[i][j - 1]);
				}
			}
		}

		return table;
	}

	static std::vector<diff_internal::UnifiedItem>
	walkLcsToUnifiedItems(const diff_internal::Lines& left, const diff_internal::Lines& right,
	                      const diff_internal::LcsTable& table) {
		std::vector<diff_internal::UnifiedItem> items;
		int ai = static_cast<int>(left.size());
		int bj = static_cast<int>(right.size());
		while (ai > 0 || bj > 0) {
			if (ai > 0 && bj > 0 && left[ai - 1] == right[bj - 1]) {
				items.push_back({ ' ', ai, bj, left[ai - 1] });
				--ai;
				--bj;
			} else if (bj > 0 && (ai == 0 || table[ai][bj - 1] >= table[ai - 1][bj])) {
				items.push_back({ '+', 0, bj, right[bj - 1] });
				--bj;
			} else {
				items.push_back({ '-', ai, 0, left[ai - 1] });
				--ai;
			}
		}

		std::reverse(items.begin(), items.end());
		return items;
	}

	// Two changes closer than twice the context belong to one hunk.
	static int findHunkLastChange(const std::vector<diff_internal::UnifiedItem>& items,
	                              int i, int context) {
		const int count = static_cast<int>(items.size());
		int last_change = i;
		int j = i + 1;
		while (j < count) {
			if (items[j].kind != ' ') {
				last_change = j;
				++j;
				continue;
			}

			int run = 0;
			int k = j;
			while (k < count && items[k].kind == ' ' && run < 2 * context) {
				++run;
				++k;
			}

			if (k >= count || items[k].kind == ' ') break;
			j = k;
		}

		return last_change;
	}

	static void emitUnifiedHunk(const std::vector<diff_internal::UnifiedItem>& items,
	                            int hunk_start, int hunk_end) {
		int a_start = 0;
		int a_count = 0;
		int b_start = 0;
		int b_count = 0;
		for (int k = hunk_start; k <= hunk_end; ++k) {
			if (items[k].kind != '+') {
				if (a_count == 0) a_start = items[k].a_line;
				++a_count;
			}

			if (items[k].kind != '-') {
				if (b_count == 0) b_start = items[k].b_line;
				++b_count;
			}
		}

		std::printf("@@ -%d,%d +%d,%d @@\n",
			a_count == 0 ? 0 : a_start, a_count,
			b_count == 0 ? 0 : b_start, b_count);
		for (int k = hunk_start; k <= hunk_end; ++k) {
			std::putchar(items[k].kind);
			std::printf("%s\n", items[k].text.c_str());
		}
	}

	static int emitUnifiedDiff(const diff_internal::Lines& left, const diff_internal::Lines& right,
	                           const diff_internal::LcsTable& table,
	                           const diff_internal::DiffOptions& opts) {
		const auto items = walkLcsToUnifiedItems(left, right, table);
		std::printf("--- %s\n+++ %s\n", opts.files[0].c_str(), opts.files[1].c_str());

		const int count = static_cast<int>(items.size());
		int i = 0;
		while (i < count) {
			while (i < count && items[i].kind == ' ') ++i;
			if (i >= count) break;

			const int last_change = findHunkLastChange(items, i, opts.context);
			const int hunk_start = (std::max)(0, i - opts.context);
			const int hunk_end = (std::min)(count - 1, last_change + opts.context);
			emitUnifiedHunk(items, hunk_start, hunk_end);
			i = hunk_end + 1;
		}

		return 1;
	}

	static std::vector<diff_internal::NormalHunk>
	buildNormalHunks(const diff_internal::Lines& left, const diff_internal::Lines& right,
	                 const diff_internal::LcsTable& table) {
		std::vector<diff_internal::NormalHunk> hunks;
		std::size_t i = left.size();
		std::size_t j = right.size();
		while (i > 0 || j > 0) {
			if (i > 0 && j > 0 && left[i - 1] == right[j - 1]) {
				--i;
				--j;
				continue;
			}

			const std::size_t end_i = i;
			const std::size_t end_j = j;
			while (i > 0 && j > 0 && left[i - 1] != right[j - 1]) {
				if (table[i - 1][j] >= table[i][j - 1]) --i;
				else                                    --j;
			}

			while (i > 0 && (j == 0 || table[i - 1][j] >= table[i][j])) --i;
			while (j > 0 && (i == 0 || table[i][j - 1] >  table[i][j])) --j;

			int a1 = static_cast<int>(i + 1);
			const int a2 = static_cast<int>(end_i);
			int b1 = static_cast<int>(j + 1);
			const int b2 = static_cast<int>(end_j);
			char kind = 'c';
			if (a1 > a2 && b1 <= b2) {
				kind = 'a';
				--a1;
			} else if (b1 > b2 && a1 <= a2) {
				kind = 'd';
				--b1;
			}

			hunks.push_back({ a1, a2, b1, b2, kind });
		}

		std::reverse(hunks.begin(), hunks.end());
		return hunks;
	}

	static void printHunkLines(const diff_internal::Lines& lines, int from, int to,
	                           const char* prefix) {
		for (int k = from; k <= to; ++k) {
			if (k - 1 >= static_cast<int>(lines.size())) continue;
			std::printf("%s%s\n", prefix, lines[k - 1].c_str());
		}
	}

	static int emitNormalDiff(const diff_internal::Lines& left, const diff_internal::Lines& right,
	                          const diff_internal::LcsTable& table) {
		for (const auto& hunk : buildNormalHunks(left, right, table)) {
			diffEmitRange(hunk.a1, hunk.a2);
			std::putchar(hunk.kind);
			diffEmitRange(hunk.b1, hunk.b2);
			std::putchar('\n');

			if (hunk.kind == 'd' || hunk.kind == 'c') printHunkLines(left, hunk.a1, hunk.a2, "< ");
			if (hunk.kind == 'c') std::puts("---");
			if (hunk.kind == 'a' || hunk.kind == 'c') printHunkLines(right, hunk.b1, hunk.b2, "> ");
		}

		return 1;
	}

	static int builtin_diff(Executor& exec, const std::vector<std::string>& args) {
		const diff_internal::DiffOptions opts = parseDiffArgs(args);
		if (opts.files.size() < 2) {
			perr("diff", "usage: diff [-q] FILE1 FILE2");
			return 2;
		}

		diff_internal::Lines left;
		diff_internal::Lines right;
		if (!readAllLines(exec, opts.files[0], left)) return 2;
		if (!readAllLines(exec, opts.files[1], right)) return 2;
		if (left == right) return 0;

		if (opts.brief) {
			std::printf("Files %s and %s differ\n", opts.files[0].c_str(), opts.files[1].c_str());
			return 1;
		}

		const diff_internal::LcsTable table = buildLcsTable(left, right);
		if (opts.unified) return emitUnifiedDiff(left, right, table, opts);
		return emitNormalDiff(left, right, table);
	}

	static std::uintmax_t treeSize(const fs::path& path, std::error_code& ec) {
		if (fs::is_regular_file(path, ec)) return fs::file_size(path, ec);
		if (!fs::is_directory(path, ec)) return 0;

		fs::recursive_directory_iterator it(path,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return 0;

		std::uintmax_t total = 0;
		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;

			std::error_code file_ec;
			if (!cur->is_regular_file(file_ec)) continue;

			const std::uintmax_t size = cur->file_size(file_ec);
			if (!file_ec) total += size;
		}

		return total;
	}

	struct DuOptions {
		bool summary = false;
		bool human = false;
		bool all = false;
		std::vector<std::string> paths;
	};

	static std::string duFormatSize(std::uintmax_t bytes, bool human) {
		if (human) return humanSize(bytes);
		return std::to_string((bytes + 1023) / 1024);
	}

	static void printDuLine(std::uintmax_t bytes, const std::string& label, bool human) {
		std::printf("%s\t%s\n", duFormatSize(bytes, human).c_str(), label.c_str());
	}

	static int duEmitDirectory(const fs::path& native, const std::string& label,
	                           const DuOptions& opts) {
		std::error_code ec;
		fs::recursive_directory_iterator it(native,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) {
			perr("du", ec.message());
			return 1;
		}

		std::uintmax_t grand = 0;
		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;

			std::error_code file_ec;
			if (!cur->is_regular_file(file_ec)) continue;

			const std::uintmax_t size = cur->file_size(file_ec);
			if (!file_ec) grand += size;
			if (opts.all) printDuLine(size, pathToUtf8(cur->path()), opts.human);
		}

		printDuLine(grand, label, opts.human);
		return 0;
	}

	static DuOptions parseDuArgs(const std::vector<std::string>& args) {
		DuOptions opts;
		for (const auto& arg : args) {
			if (arg == "-s" || arg == "--summarize") opts.summary = true;
			else if (arg == "-h" || arg == "--human-readable") opts.human = true;
			else if (arg == "-a" || arg == "--all") opts.all = true;
			else if (startsWithDash(arg)) continue;
			else opts.paths.push_back(arg);
		}

		if (opts.paths.empty()) opts.paths.push_back(".");
		return opts;
	}

	static int duOnePath(Executor& exec, const std::string& path, const DuOptions& opts) {
		const fs::path native = toNative(exec, path);
		std::error_code ec;
		if (!fs::exists(native, ec)) {
			perr("du", path, ec);
			return 1;
		}

		if (opts.summary) {
			printDuLine(treeSize(native, ec), path, opts.human);
			return 0;
		}

		if (fs::is_regular_file(native, ec)) {
			printDuLine(fs::file_size(native, ec), path, opts.human);
			return 0;
		}

		return duEmitDirectory(native, path, opts);
	}

	static int builtin_du(Executor& exec, const std::vector<std::string>& args) {
		const DuOptions opts = parseDuArgs(args);
		int status = 0;
		for (const auto& path : opts.paths) status |= duOnePath(exec, path, opts);
		return status;
	}

	static std::vector<std::string> logicalDriveRoots() {
		std::vector<std::string> drives;
#ifdef _WIN32
		const DWORD mask = ::GetLogicalDrives();
		for (int i = 0; i < 26; ++i) {
			if ((mask & (1u << i)) == 0) continue;
			const char root[4] = { static_cast<char>('A' + i), ':', '\\', 0 };
			drives.push_back(root);
		}
#endif
		return drives;
	}

	static std::string dfFormatSize(std::uintmax_t bytes, bool human) {
		if (human) return humanSize(bytes);
		return std::to_string(bytes / 1024);
	}

	static bool printDfRow(const std::string& drive, bool human) {
#ifdef _WIN32
		std::string root = drive;
		if (!root.empty() && root.back() != '\\' && root.back() != '/') root.push_back('\\');

		ULARGE_INTEGER avail{};
		ULARGE_INTEGER total{};
		ULARGE_INTEGER free_bytes{};
		if (::GetDiskFreeSpaceExA(root.c_str(), &avail, &total, &free_bytes) == 0) return false;

		const std::uintmax_t total_bytes = total.QuadPart;
		const std::uintmax_t avail_bytes = avail.QuadPart;
		const std::uintmax_t used_bytes = total_bytes > avail_bytes ? total_bytes - avail_bytes : 0;
		const int percent = total_bytes == 0
			? 0
			: static_cast<int>((used_bytes * 100) / total_bytes);
		std::printf("%-20s %12s %12s %12s %4d%% %s\n",
			root.c_str(), dfFormatSize(total_bytes, human).c_str(),
			dfFormatSize(used_bytes, human).c_str(), dfFormatSize(avail_bytes, human).c_str(),
			percent, root.c_str());
#else
		(void)drive;
		(void)human;
#endif
		return true;
	}

	static int builtin_df(Executor& exec, const std::vector<std::string>& args) {
		bool human = false;
		std::vector<std::string> paths;
		for (const auto& arg : args) {
			if (arg == "-h" || arg == "--human-readable") human = true;
			else if (startsWithDash(arg)) continue;
			else paths.push_back(arg);
		}

		std::vector<std::string> drives;
		if (paths.empty()) drives = logicalDriveRoots();
		for (const auto& path : paths) drives.push_back(pathToUtf8(toNative(exec, path)));

		std::printf("%-20s %12s %12s %12s %5s %s\n",
			"Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on");
		int status = 0;
		for (const auto& drive : drives) {
			if (!printDfRow(drive, human)) status = 1;
		}

		return status;
	}

	static const char* statTypeName(const struct stat& info) {
#ifdef S_ISDIR
		if (S_ISDIR(info.st_mode)) return "directory";
		if (S_ISREG(info.st_mode)) return "regular file";
#else
		if ((info.st_mode & S_IFMT) == S_IFDIR) return "directory";
		if ((info.st_mode & S_IFMT) == S_IFREG) return "regular file";
#endif
		return "special file";
	}

	static bool statOnePath(Executor& exec, const std::string& path) {
		const std::string native = exec.pathConv().toWin32(path);
		struct stat info {};
		if (::stat(native.c_str(), &info) != 0) {
			perr("stat", withErrno(path));
			return false;
		}

		const std::string modified = formatTime(brokenDownTime(info.st_mtime, false),
			"%Y-%m-%d %H:%M:%S");
		std::printf("  File: %s\n", path.c_str());
		std::printf("  Size: %lld\tType: %s\n",
			static_cast<long long>(info.st_size), statTypeName(info));
		std::printf("Access: (%04o)\n", static_cast<unsigned int>(info.st_mode & 0777));
		std::printf("Modify: %s\n", modified.c_str());
		return true;
	}

	static int builtin_stat(Executor& exec, const std::vector<std::string>& args) {
		if (args.empty()) {
			perr("stat", "missing operand");
			return 1;
		}

		int status = 0;
		for (const auto& path : args) {
			if (startsWithDash(path)) continue;
			if (!statOnePath(exec, path)) status = 1;
		}

		return status;
	}

	// Only the owner write bit is honoured: it maps onto the read-only
	// attribute, the one permission Windows files actually carry.
	static bool chmodWantsWritable(const std::string& mode) {
		if (mode == "-w" || mode == "u-w" || mode == "a-w" || mode == "go-w") return false;
		if (mode == "+w" || mode == "u+w" || mode == "a+w" || mode == "go+w") return true;

		const bool octal = mode.size() == 3
			&& isDigitChar(mode[0]) && isDigitChar(mode[1]) && isDigitChar(mode[2]);
		if (octal) return ((mode[0] - '0') & 2) != 0;
		return true;
	}

	static bool chmodOnePath(Executor& exec, const std::string& path, bool writable) {
		const std::string native = exec.pathConv().toWin32(path);
#ifdef _WIN32
		DWORD attributes = ::GetFileAttributesA(native.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES) {
			perr("chmod", path + ": not found");
			return false;
		}

		if (writable) attributes &= ~FILE_ATTRIBUTE_READONLY;
		else          attributes |= FILE_ATTRIBUTE_READONLY;
		if (::SetFileAttributesA(native.c_str(), attributes) != 0) return true;

		perr("chmod", path + ": cannot change");
		return false;
#else
		(void)native;
		(void)writable;
		return true;
#endif
	}

	static int builtin_chmod(Executor& exec, const std::vector<std::string>& args) {
		if (args.size() < 2) {
			perr("chmod", "usage: chmod MODE FILE...");
			return 1;
		}

		const bool writable = chmodWantsWritable(args[0]);
		int status = 0;
		for (std::size_t i = 1; i < args.size(); ++i) {
			if (!chmodOnePath(exec, args[i], writable)) status = 1;
		}

		return status;
	}

	struct LnOptions {
		bool symbolic = false;
		bool force = false;
		std::vector<std::string> operands;
	};

	static LnOptions parseLnArgs(const std::vector<std::string>& args) {
		LnOptions opts;
		for (const auto& arg : args) {
			if (arg == "-s" || arg == "--symbolic") {
				opts.symbolic = true;
			} else if (arg == "-f" || arg == "--force") {
				opts.force = true;
			} else if (arg == "-sf" || arg == "-fs") {
				opts.symbolic = true;
				opts.force = true;
			} else if (!startsWithDash(arg)) {
				opts.operands.push_back(arg);
			}
		}

		return opts;
	}

#ifdef _WIN32
	static bool createWindowsLink(const std::string& native_target, const std::string& native_link,
	                              bool symbolic) {
		if (!symbolic) {
			return ::CreateHardLinkA(native_link.c_str(), native_target.c_str(), nullptr) != 0;
		}

		DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
		std::error_code ec;
		if (fs::is_directory(native_target, ec)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
		return ::CreateSymbolicLinkA(native_link.c_str(), native_target.c_str(), flags) != 0;
	}
#endif

	static int builtin_ln(Executor& exec, const std::vector<std::string>& args) {
		const LnOptions opts = parseLnArgs(args);
		if (opts.operands.size() < 2) {
			perr("ln", "usage: ln [-s] [-f] TARGET LINK");
			return 1;
		}

#ifdef _WIN32
		const std::string& target = opts.operands[0];
		const std::string& link = opts.operands[1];
		const std::string native_target = exec.pathConv().toWin32(target);
		const std::string native_link = exec.pathConv().toWin32(link);
		if (opts.force) ::DeleteFileA(native_link.c_str());
		if (createWindowsLink(native_target, native_link, opts.symbolic)) return 0;

		std::fprintf(stderr, "wbsh: ln: %s -> %s failed (err=%lu)\n",
			link.c_str(), target.c_str(), ::GetLastError());
		return 1;
#else
		(void)exec;
		perr("ln", "not supported");
		return 1;
#endif
	}

	static std::string currentCwdPosix(Executor& exec) {
		std::error_code ec;
		const fs::path cwd = fs::current_path(ec);
		if (ec) return exec.env().get("PWD");
		return exec.pathConv().toPosix(pathToUtf8(cwd));
	}

	static void printDirStack(Executor& exec, bool numbered) {
		const auto& stack = exec.dirStack();
		if (stack.empty()) {
			std::printf("%s\n", currentCwdPosix(exec).c_str());
			return;
		}

		if (numbered) {
			for (std::size_t i = 0; i < stack.size(); ++i) {
				std::printf("%2zu  %s\n", i, stack[i].c_str());
			}

			return;
		}

		std::printf("%s\n", joinWords(stack).c_str());
	}

	static bool pushdEnter(Executor& exec, const std::string& target,
	                       std::vector<std::string>& stack) {
		std::error_code ec;
		fs::current_path(toNative(exec, target), ec);
		if (ec) {
			perr("pushd", target, ec);
			return false;
		}

		stack.insert(stack.begin(), exec.pathConv().toPosix(pathToUtf8(fs::current_path(ec))));
		return true;
	}

	static int builtin_pushd(Executor& exec, const std::vector<std::string>& args) {
		auto& stack = exec.dirStack();
		const std::string previous = currentCwdPosix(exec);
		if (stack.empty()) stack.push_back(previous);

		if (args.empty()) {
			if (stack.size() < 2) {
				perr("pushd", "no other directory");
				return 1;
			}

			std::swap(stack[0], stack[1]);
		} else if (!pushdEnter(exec, args[0], stack)) {
			return 1;
		}

		std::error_code ec;
		fs::current_path(toNative(exec, stack[0]), ec);
		if (!ec) {
			exec.env().set("OLDPWD", previous);
			exec.env().set("PWD", stack[0]);
		}

		printDirStack(exec, false);
		return 0;
	}

	static int builtin_popd(Executor& exec, const std::vector<std::string>&) {
		auto& stack = exec.dirStack();
		if (stack.size() < 2) {
			perr("popd", "directory stack empty");
			return 1;
		}

		const std::string previous = stack.front();
		stack.erase(stack.begin());

		std::error_code ec;
		fs::current_path(exec.pathConv().toWin32(stack[0]), ec);
		if (ec) {
			perr("popd", stack[0], ec);
			return 1;
		}

		exec.env().set("OLDPWD", previous);
		exec.env().set("PWD", stack[0]);
		printDirStack(exec, false);
		return 0;
	}

	static int builtin_dirs(Executor& exec, const std::vector<std::string>& args) {
		bool numbered = false;
		bool clear = false;
		for (const auto& arg : args) {
			if (arg == "-l") continue;   // long form: accepted, not implemented
			if (arg == "-v") numbered = true;
			else if (arg == "-c") clear = true;
			else if (arg == "-p") numbered = false;
		}

		if (clear) {
			exec.dirStack().clear();
			return 0;
		}

		if (exec.dirStack().empty()) exec.dirStack().push_back(currentCwdPosix(exec));
		printDirStack(exec, numbered);
		return 0;
	}

	static int builtin_yes(Executor&, const std::vector<std::string>& args) {
		std::string line = args.empty() ? "y" : joinWords(args);
		line.push_back('\n');
		for (;;) {
			if (std::fwrite(line.data(), 1, line.size(), stdout) != line.size()) {
				return 1;   // pipe closed (downstream done) — exit cleanly
			}
		}
	}

	// --all / --ignore=N are accepted and ignored.
	static int builtin_nproc(Executor&, const std::vector<std::string>&) {
		unsigned count = std::thread::hardware_concurrency();
		if (count == 0) count = 1;
		std::printf("%u\n", count);
		return 0;
	}

	static void tputConsoleSize(int& columns, int& rows) {
		if (queryConsoleSize(columns, rows)) return;
		columns = kFallbackConsoleColumns;
		rows = kFallbackConsoleRows;
	}

	static const char* tputStaticEscape(const std::string& cap) {
		static const std::unordered_map<std::string, std::string> kEscapes = {
			{ "clear", "\x1b[2J\x1b[H" },
			{ "reset", "\033c"          },
			{ "bold",  "\x1b[1m"        },
			{ "dim",   "\x1b[2m"        },
			{ "smul",  "\x1b[4m"        },
			{ "rmul",  "\x1b[24m"       },
			{ "rev",   "\x1b[7m"        },
			{ "blink", "\x1b[5m"        },
			{ "sgr0",  "\x1b[0m"        },
			{ "op",    "\x1b[0m"        },
			{ "civis", "\x1b[?25l"      },
			{ "cnorm", "\x1b[?25h"      },
			{ "el",    "\x1b[K"         },
			{ "ed",    "\x1b[J"         },
			{ "home",  "\x1b[H"         },
		};
		const auto it = kEscapes.find(cap);
		return (it == kEscapes.end()) ? nullptr : it->second.c_str();
	}

	static int tputCursorPosition(const std::vector<std::string>& args) {
		int row = 0;
		int column = 0;
		if (!parseInt(args[1], row) || !parseInt(args[2], column)) return 1;
		std::printf("\x1b[%d;%dH", row + 1, column + 1);
		return 0;
	}

	static int tputColor(const std::string& value, int base, int reset) {
		int color = 0;
		if (!parseInt(value, color)) return 1;
		if (color >= 0 && color < 8) std::printf("\x1b[%dm", base + color);
		else                         std::printf("\x1b[%dm", reset);
		return 0;
	}

	static int tputParameterizedCap(const std::string& cap, const std::vector<std::string>& args) {
		const bool foreground = cap == "setaf" || cap == "setf";
		const bool background = cap == "setab" || cap == "setb";
		if (cap == "cup" && args.size() >= 3) return tputCursorPosition(args);
		if (foreground && args.size() >= 2) return tputColor(args[1], 30, 39);
		if (background && args.size() >= 2) return tputColor(args[1], 40, 49);
		return -1;
	}

	static int builtin_tput(Executor&, const std::vector<std::string>& args) {
		if (args.empty()) return 0;

		const std::string& cap = args[0];
		if (cap == "cols" || cap == "columns" || cap == "lines") {
			int columns = 0;
			int rows = 0;
			tputConsoleSize(columns, rows);
			std::printf("%d\n", cap == "lines" ? rows : columns);
			return 0;
		}

		const char* escape = tputStaticEscape(cap);
		if (escape != nullptr) {
			std::fputs(escape, stdout);
			return 0;
		}

		const int status = tputParameterizedCap(cap, args);
		if (status >= 0) return status;

		std::fprintf(stderr, "wbsh: tput: unknown capability: %s\n", cap.c_str());
		return 1;
	}

	namespace mktemp_internal {
		struct MktempOptions {
			bool make_dir = false;
			bool dry_run = false;
			bool quiet = false;
			std::string template_arg;
			std::string tmpdir_override;
		};
	}  // namespace mktemp_internal

	static int parseMktempArgs(const std::vector<std::string>& args,
	                           mktemp_internal::MktempOptions& opts) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-t") continue;   // legacy: use TMPDIR, which is already the default
			if      (arg == "-d" || arg == "--directory") opts.make_dir = true;
			else if (arg == "-u" || arg == "--dry-run")   opts.dry_run = true;
			else if (arg == "-q" || arg == "--quiet")     opts.quiet = true;
			else if (arg == "-p" || arg == "--tmpdir") {
				if (i + 1 < args.size()) opts.tmpdir_override = args[++i];
			} else if (arg.size() > 9 && arg.compare(0, 9, "--tmpdir=") == 0) {
				opts.tmpdir_override = arg.substr(9);
			} else if (isOptionNotStdin(arg)) {
				std::fprintf(stderr, "wbsh: mktemp: unknown option: %s\n", arg.c_str());
				return 1;
			} else if (opts.template_arg.empty()) {
				opts.template_arg = arg;
			}
		}

		if (opts.template_arg.empty()) opts.template_arg = "tmp.XXXXXXXXXX";
		return 0;
	}

	static std::string resolveMktempBaseDir(Executor& exec,
	                                        const std::string& tmpdir_override) {
		if (!tmpdir_override.empty()) return tmpdir_override;
		if (!exec.env().get("TMPDIR").empty()) return exec.env().get("TMPDIR");
		if (!exec.env().get("TEMP").empty())   return exec.env().get("TEMP");
		if (!exec.env().get("TMP").empty())    return exec.env().get("TMP");
		return "/tmp";
	}

	static fs::path resolveMktempFullPath(Executor& exec,
	                                      const std::string& template_arg,
	                                      const std::string& base_dir) {
		if (template_arg.find('/') != std::string::npos
		    || template_arg.find('\\') != std::string::npos) {
			return fs::path(toNative(exec, template_arg));
		}

		return fs::path(toNative(exec, base_dir)) / template_arg;
	}

	static std::string randomMktempSuffix(std::size_t xcount, int attempt) {
		static const char* const kAlphabet =
			"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
		constexpr std::size_t kAlphabetSize = 62;

		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		unsigned long long mix = static_cast<unsigned long long>(ticks)
			^ (static_cast<unsigned long long>(::GetCurrentProcessId()) << 32)
			^ static_cast<unsigned long long>(attempt) * 0x9E3779B97F4A7C15ULL;

		std::string suffix(xcount, 'X');
		for (std::size_t k = 0; k < xcount; ++k) {
			suffix[k] = kAlphabet[mix % kAlphabetSize];
			mix = mix * 6364136223846793005ULL + 1442695040888963407ULL;
		}

		return suffix;
	}

	static bool tryClaimMktempCandidate(const std::string& candidate, bool make_dir) {
		std::error_code ec;
		if (make_dir) return fs::create_directory(candidate, ec) && !ec;

		const HANDLE handle = ::CreateFileA(candidate.c_str(),
			GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;

		::CloseHandle(handle);
		return true;
	}

	static std::size_t countTrailingPlaceholders(const std::string& text) {
		std::size_t count = 0;
		while (count < text.size() && text[text.size() - 1 - count] == 'X') ++count;
		return count;
	}

	static bool claimMktempPath(Executor& exec, const std::string& prefix, std::size_t xcount,
	                            const mktemp_internal::MktempOptions& opts) {
		for (int attempt = 0; attempt < kMktempAttempts; ++attempt) {
			const std::string candidate = prefix + randomMktempSuffix(xcount, attempt);
			if (!opts.dry_run && !tryClaimMktempCandidate(candidate, opts.make_dir)) continue;

			std::printf("%s\n", exec.pathConv().toPosix(candidate).c_str());
			return true;
		}

		return false;
	}

	static int builtin_mktemp(Executor& exec, const std::vector<std::string>& args) {
		mktemp_internal::MktempOptions opts;
		const int parse_status = parseMktempArgs(args, opts);
		if (parse_status != 0) return parse_status;

		const std::string base = resolveMktempBaseDir(exec, opts.tmpdir_override);
		const std::string full = pathToUtf8(resolveMktempFullPath(exec, opts.template_arg, base));
		const std::size_t xcount = countTrailingPlaceholders(full);
		if (xcount < kMktempMinPlaceholders) {
			if (!opts.quiet) {
				std::fprintf(stderr, "wbsh: mktemp: too few X's in template '%s'\n",
					opts.template_arg.c_str());
			}

			return 1;
		}

		if (claimMktempPath(exec, full.substr(0, full.size() - xcount), xcount, opts)) return 0;

		if (!opts.quiet) {
			std::fprintf(stderr, "wbsh: mktemp: failed to create unique file from '%s'\n",
				opts.template_arg.c_str());
		}

		return 1;
	}

	static void killPrintSignalList() {
		std::printf(" 1) HUP   2) INT   3) QUIT  4) ILL   "
			"5) TRAP  6) ABRT  7) BUS   8) FPE\n"
			" 9) KILL 10) USR1 11) SEGV 12) USR2 "
			"13) PIPE 14) ALRM 15) TERM\n");
	}

	static bool killOneProcess(int pid, int signum) {
#ifdef _WIN32
		const HANDLE process = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
		if (process == nullptr) {
			std::fprintf(stderr, "wbsh: kill: %d: no such process\n", pid);
			return false;
		}

		const bool terminated = ::TerminateProcess(process, static_cast<UINT>(128 + signum)) != 0;
		if (!terminated) std::fprintf(stderr, "wbsh: kill: %d: cannot terminate\n", pid);
		::CloseHandle(process);
		return terminated;
#else
		(void)pid;
		(void)signum;
		return true;
#endif
	}

	static int killTerminate(const std::vector<int>& pids, int signum) {
		int status = 0;
		for (int pid : pids) {
			if (!killOneProcess(pid, signum)) status = 1;
		}

		return status;
	}

	static bool killSignalByName(const std::string& arg, int& signum) {
		if (arg == "-KILL") { signum = 9;  return true; }
		if (arg == "-TERM") { signum = 15; return true; }
		if (arg == "-INT")  { signum = 2;  return true; }
		if (arg == "-HUP")  { signum = 1;  return true; }
		return false;
	}

	static int builtin_kill(Executor&, const std::vector<std::string>& args) {
		int signum = kSignalTerm;
		std::vector<int> pids;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-l") {
				killPrintSignalList();
				return 0;
			}

			if (arg == "-s" && i + 1 < args.size()) {
				parseInt(args[++i], signum);
				continue;
			}

			if (arg.size() > 1 && arg[0] == '-' && isDigitChar(arg[1])) {
				parseInt(arg.substr(1), signum);
				continue;
			}

			if (killSignalByName(arg, signum)) continue;

			int pid = 0;
			if (!parseInt(arg, pid)) {
				std::fprintf(stderr, "wbsh: kill: %s: arguments must be PIDs\n", arg.c_str());
				return 1;
			}

			pids.push_back(pid);
		}

		if (pids.empty()) {
			perr("kill", "usage: kill [-SIG] PID...");
			return 2;
		}

		return killTerminate(pids, signum);
	}

	static void registerFileBuiltins(Executor& exec) {
		exec.registerBuiltin("ls",       builtin_ls);
		exec.registerBuiltin("cat",      builtin_cat);
		exec.registerBuiltin("clear",    builtin_clear);
		exec.registerBuiltin("which",    builtin_which);
		exec.registerBuiltin("mkdir",    builtin_mkdir);
		exec.registerBuiltin("rmdir",    builtin_rmdir);
		exec.registerBuiltin("rm",       builtin_rm);
		exec.registerBuiltin("cp",       builtin_cp);
		exec.registerBuiltin("mv",       builtin_mv);
		exec.registerBuiltin("touch",    builtin_touch);
		exec.registerBuiltin("head",     builtin_head);
		exec.registerBuiltin("tail",     builtin_tail);
		exec.registerBuiltin("wc",       builtin_wc);
		exec.registerBuiltin("stat",     builtin_stat);
		exec.registerBuiltin("chmod",    builtin_chmod);
		exec.registerBuiltin("ln",       builtin_ln);
		exec.registerBuiltin("cmp",      builtin_cmp);
		exec.registerBuiltin("diff",     builtin_diff);
		exec.registerBuiltin("du",       builtin_du);
		exec.registerBuiltin("df",       builtin_df);
		exec.registerBuiltin("realpath", builtin_realpath);
		exec.registerBuiltin("readlink", builtin_readlink);
		exec.registerBuiltin("basename", builtin_basename);
		exec.registerBuiltin("dirname",  builtin_dirname);
		exec.registerBuiltin("pushd",    builtin_pushd);
		exec.registerBuiltin("popd",     builtin_popd);
		exec.registerBuiltin("dirs",     builtin_dirs);
	}

	static void registerSystemBuiltins(Executor& exec) {
		exec.registerBuiltin("whoami",   builtin_whoami);
		exec.registerBuiltin("hostname", builtin_hostname);
		exec.registerBuiltin("env",      builtin_env);
		exec.registerBuiltin("sleep",    builtin_sleep);
		exec.registerBuiltin("date",     builtin_date);
		exec.registerBuiltin("seq",      builtin_seq);
		exec.registerBuiltin("uname",    builtin_uname);
		exec.registerBuiltin("id",       builtin_id);
		exec.registerBuiltin("expr",     builtin_expr);
		exec.registerBuiltin("yes",      builtin_yes);
		exec.registerBuiltin("nproc",    builtin_nproc);
		exec.registerBuiltin("tput",     builtin_tput);
		exec.registerBuiltin("mktemp",   builtin_mktemp);
		exec.registerBuiltin("kill",     builtin_kill);
	}

	void registerCoreutils(Executor& exec) {
		registerFileBuiltins(exec);
		registerTextBuiltins(exec);
		registerEncodingBuiltins(exec);
		registerArchiveBuiltins(exec);
		registerSystemBuiltins(exec);
		registerBcBuiltin(exec);
		registerHashBuiltins(exec);
		registerCurlBuiltin(exec);
		registerFzfBuiltin(exec);
		registerTmuxBuiltin(exec);
		registerUtilsBuiltin(exec);
	}

}  // namespace wbsh
