/**
 * @file coreutils_stat.cpp
 * @brief `touch` and `stat`: reading and setting file metadata.
 *
 * Windows has no uid / gid or mode bits; like `ls -l` these report the
 * current user (1000) and permissions derived from the read-only
 * attribute and the executable extensions.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dateparse.h"
#include "executor.h"
#include "filetimes.h"
#include "optscan.h"
#include "pathconv.h"
#include "timefmt.h"

namespace fs = std::filesystem;

namespace wbsh::touch_detail {

	enum OptId { kAccess, kModify, kNoCreate, kDate, kStamp, kReference, kTime, kNoDeref, kForce };

	static const std::vector<OptDef> kTouchOptions = {
		{ kAccess,    'a', nullptr,          OptArg::None },
		{ kModify,    'm', nullptr,          OptArg::None },
		{ kNoCreate,  'c', "no-create",      OptArg::None },
		{ kDate,      'd', "date",           OptArg::Required },
		{ kStamp,     't', nullptr,          OptArg::Required },
		{ kReference, 'r', "reference",      OptArg::Required },
		{ kTime,      0,   "time",           OptArg::Required },
		{ kNoDeref,   'h', "no-dereference", OptArg::None },
		{ kForce,     'f', nullptr,          OptArg::None },
	};

	struct Options {
		bool access = false;
		bool modify = false;
		bool no_create = false;
		bool has_date = false;
		std::string date;
		bool has_stamp = false;
		std::string stamp;
		std::string reference;
		std::vector<std::string> files;
	};

	static bool applyTimeWord(const std::string& word, Options& options) {
		if (word == "access" || word == "atime" || word == "use") options.access = true;
		else if (word == "modify" || word == "mtime") options.modify = true;
		else {
			perr("touch", "invalid argument '" + word + "' for '--time'");
			return false;
		}

		return true;
	}

	static bool applyOption(const OptHit& hit, Options& options) {
		switch (hit.id) {
			case kAccess:
				options.access = true;
				return true;
			case kModify:
				options.modify = true;
				return true;
			case kNoCreate:
				options.no_create = true;
				return true;
			case kDate:
				options.has_date = true;
				options.date = hit.value;
				return true;
			case kStamp:
				options.has_stamp = true;
				options.stamp = hit.value;
				return true;
			case kReference:
				options.reference = hit.value;
				return true;
			case kTime:
				return applyTimeWord(hit.value, options);
			default:
				return true;
		}
	}

	static bool parseOptions(const std::vector<std::string>& args, Options& options) {
		OptScanResult scanned;
		if (!scanOptions("touch", args, kTouchOptions, scanned)) return false;

		for (const OptHit& hit : scanned.hits) {
			if (!applyOption(hit, options)) return false;
		}

		options.files = scanned.operands;
		if (!options.access && !options.modify) options.access = options.modify = true;
		return true;
	}

	static DateValue currentTime() {
		using namespace std::chrono;
		const auto since_epoch = system_clock::now().time_since_epoch();
		const long long nanos = duration_cast<nanoseconds>(since_epoch).count();
		return { nanos / 1000000000LL, static_cast<long>(nanos % 1000000000LL) };
	}

	static bool referenceTimes(Executor& exec, const std::string& reference, FileTimes& times) {
		if (readFileTimes(toNative(exec, reference), true, times)) return true;

		perr("touch", "failed to get attributes of '" + reference + "': No such file or directory");
		return false;
	}

	// The access and modification times to set; -r supplies both, -d / -t
	// one point in time (with -r, -d is read relative to the reference).
	static bool targetTimes(Executor& exec, const Options& options, FileTimes& times) {
		const DateValue now = currentTime();
		times.access = times.modify = now;
		const bool has_reference = !options.reference.empty();
		if (has_reference && !referenceTimes(exec, options.reference, times)) return false;

		const std::time_t base = static_cast<std::time_t>(
			options.reference.empty() ? now.seconds : times.modify.seconds);
		DateValue value;
		if (options.has_date && !parseDateString(options.date, base, value)) {
			perr("touch", "invalid date format '" + options.date + "'");
			return false;
		}

		if (options.has_stamp && !parseTouchStamp(options.stamp, base, value)) {
			perr("touch", "invalid date format '" + options.stamp + "'");
			return false;
		}

		if (options.has_date || options.has_stamp) times.access = times.modify = value;
		return true;
	}

	// false: the file does not exist and must not be created (-c).
	static bool ensureExists(const fs::path& native, const std::string& path,
			const Options& options, bool& ok) {
		std::error_code ec;
		if (fs::exists(native, ec)) return true;
		if (options.no_create) return false;

		std::ofstream created(native, std::ios::binary | std::ios::app);
		if (created) return true;

		perr("touch", "cannot touch '" + path + "': No such file or directory");
		ok = false;
		return false;
	}

	static bool touchOne(Executor& exec, const std::string& path, const Options& options,
			const FileTimes& times) {
		if (path == "-") return true;

		const fs::path native = toNative(exec, path);
		bool ok = true;
		if (!ensureExists(native, path, options, ok)) return ok;

		const DateValue* access = options.access ? &times.access : nullptr;
		const DateValue* modify = options.modify ? &times.modify : nullptr;
		if (writeFileTimes(native, access, modify)) return true;

		perr("touch", "setting times of '" + path + "': Permission denied");
		return false;
	}

	static int builtin_touch(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options)) return 1;
		if (options.files.empty()) {
			perr("touch", "missing file operand");
			return 1;
		}

		FileTimes times;
		if (!targetTimes(exec, options, times)) return 1;

		int status = 0;
		for (const std::string& path : options.files) {
			if (!touchOne(exec, path, options, times)) status = 1;
		}

		return status;
	}

}  // namespace wbsh::touch_detail

namespace wbsh::stat_detail {

	static const int kUserId = 1000;
	static const int kBlockSize = 512;
	static const int kIoBlock = 4096;

	static const char* const kDefaultFormat =
		"  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n"
		"Device: %Dh/%dd\tInode: %-11i Links: %h\n"
		"Access: (%04a/%10.10A)  Uid: (%5u/%8U)   Gid: (%5g/%8G)\n"
		"Access: %x\n"
		"Modify: %y\n"
		"Change: %z\n"
		" Birth: %w\n";
	static const char* const kTerseFormat = "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o\n";

	struct Info {
		std::string name;
		std::string user;
		std::string link_target;
		bool is_dir = false;
		bool is_link = false;
		bool read_only = false;
		bool executable = false;
		unsigned long long size = 0;
		unsigned long long device = 0;
		unsigned long long inode = 0;
		unsigned long long links = 1;
		FileTimes times;
	};

	struct Options {
		std::string format;
		bool has_format = false;
		bool interpret_escapes = false;
		bool dereference = false;
		bool terse = false;
		std::vector<std::string> files;
	};

	enum OptId { kFormat, kPrintf, kDereference, kTerse };

	static const std::vector<OptDef> kStatOptions = {
		{ kFormat,      'c', "format",      OptArg::Required },
		{ kPrintf,      0,   "printf",      OptArg::Required },
		{ kDereference, 'L', "dereference", OptArg::None },
		{ kTerse,       't', "terse",       OptArg::None },
	};

	static void applyOption(const OptHit& hit, Options& options) {
		switch (hit.id) {
			case kFormat:
				options.format = hit.value + "\n";
				options.has_format = true;
				break;
			case kPrintf:
				options.format = hit.value;
				options.has_format = true;
				options.interpret_escapes = true;
				break;
			case kDereference:
				options.dereference = true;
				break;
			default:
				options.terse = true;
				break;
		}
	}

	static bool parseOptions(const std::vector<std::string>& args, Options& options) {
		OptScanResult scanned;
		if (!scanOptions("stat", args, kStatOptions, scanned)) return false;

		for (const OptHit& hit : scanned.hits) applyOption(hit, options);
		options.files = scanned.operands;
		if (options.files.empty()) {
			perr("stat", "missing operand");
			return false;
		}

		return true;
	}

#ifdef _WIN32
	static void readHandleInfo(const fs::path& native, bool follow, Info& info) {
		DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
		if (!follow) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
		const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		const HANDLE handle = ::CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, share, nullptr,
			OPEN_EXISTING, flags, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return;

		BY_HANDLE_FILE_INFORMATION data{};
		if (::GetFileInformationByHandle(handle, &data) != FALSE) {
			info.device = data.dwVolumeSerialNumber;
			const unsigned long long high = data.nFileIndexHigh;
			info.inode = (high << 32) | data.nFileIndexLow;
			info.links = data.nNumberOfLinks;
			info.read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
		}

		::CloseHandle(handle);
	}
#else
	static void readHandleInfo(const fs::path&, bool, Info&) {}
#endif /* _WIN32 */

	static void readLinkTarget(Executor& exec, const fs::path& native, Info& info) {
		std::error_code ec;
		const fs::path target = fs::read_symlink(native, ec);
		if (!ec) info.link_target = exec.pathConv().toPosix(pathToUtf8(target));
	}

	static bool collectInfo(Executor& exec, const std::string& path, bool follow, Info& info) {
		const fs::path native = toNative(exec, path);
		std::error_code ec;
		const fs::file_status status = follow
			? fs::status(native, ec)
			: fs::symlink_status(native, ec);
		if (ec || status.type() == fs::file_type::not_found) return false;
		if (!readFileTimes(native, follow, info.times)) return false;

		info.name = path;
		info.user = currentUserName(exec);
		info.is_link = isLinkStatus(status);
		info.is_dir = !info.is_link && fs::is_directory(status);
		info.executable = info.is_dir || hasExecutableExtension(native);
		if (!info.is_dir && !info.is_link) info.size = fs::file_size(native, ec);
		if (info.is_link) readLinkTarget(exec, native, info);

		readHandleInfo(native, follow, info);
		return true;
	}

	static unsigned modeBits(const Info& info) {
		if (info.is_link) return 0777;

		const unsigned execute = info.executable ? 0111 : 0;
		const unsigned write = info.read_only ? 0 : 0200;
		return 0444 | write | execute;
	}

	static unsigned typeBits(const Info& info) {
		if (info.is_link) return 0120000;
		if (info.is_dir) return 0040000;
		return 0100000;
	}

	static std::string permissionText(const Info& info) {
		const unsigned bits = modeBits(info);
		std::string text(1, info.is_link ? 'l' : (info.is_dir ? 'd' : '-'));
		const char letters[] = { 'r', 'w', 'x' };
		for (int shift = 8; shift >= 0; --shift) {
			text.push_back((bits >> shift) & 1 ? letters[(8 - shift) % 3] : '-');
		}

		return text;
	}

	static std::string fileTypeText(const Info& info) {
		if (info.is_link) return "symbolic link";
		if (info.is_dir) return "directory";
		return info.size == 0 ? "regular empty file" : "regular file";
	}

	static std::string quotedName(const Info& info) {
		std::string text = "'" + info.name + "'";
		if (info.is_link) text += " -> '" + info.link_target + "'";
		return text;
	}

	static std::string mountPoint(Executor& exec, const std::string& path) {
		const std::string native = pathToUtf8(toNative(exec, path));
		if (native.size() < 2 || native[1] != ':') return "/";

		return exec.pathConv().toPosix(native.substr(0, 2) + "\\");
	}

	struct Directive {
		std::string flags;
		std::string width;
		int precision = -1;
		char conversion = 0;
	};

	struct Value {
		bool numeric = false;
		long long number = 0;
		char base = 'd';
		std::string text;
	};

	static Value numberValue(long long number, char base = 'd') {
		Value value;
		value.numeric = true;
		value.number = number;
		value.base = base;
		return value;
	}

	static Value textValue(std::string text) {
		Value value;
		value.text = std::move(text);
		return value;
	}

	// `%X` and friends take a precision as digits of the fractional second.
	static Value epochValue(const DateValue& when, int precision) {
		if (precision <= 0) return numberValue(when.seconds);

		char nanos[16];
		std::snprintf(nanos, sizeof(nanos), "%09ld", when.nanos);
		const std::string digits(nanos, static_cast<std::size_t>(precision < 9 ? precision : 9));
		return textValue(std::to_string(when.seconds) + "." + digits);
	}

	static const DateValue* timeFor(char conversion, const Info& info) {
		const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(conversion)));
		if (lower == 'x') return &info.times.access;
		if (lower == 'y') return &info.times.modify;
		if (lower == 'z') return &info.times.change;
		if (lower == 'w') return &info.times.birth;
		return nullptr;
	}

	// Lower case is the readable time, upper case the epoch seconds.
	static bool timeDirective(char conversion, const Info& info, int precision, Value& value) {
		const DateValue* when = timeFor(conversion, info);
		if (when == nullptr) return false;

		const bool epoch = std::isupper(static_cast<unsigned char>(conversion)) != 0;
		value = epoch ? epochValue(*when, precision) : textValue(formatFullIsoTime(*when));
		return true;
	}

	static long long blockCount(const Info& info) {
		return static_cast<long long>((info.size + kBlockSize - 1) / kBlockSize);
	}

	static bool sizeDirective(char conversion, const Info& info, Value& value) {
		const long long device = static_cast<long long>(info.device);
		if (conversion == 's')      value = numberValue(static_cast<long long>(info.size));
		else if (conversion == 'b') value = numberValue(blockCount(info));
		else if (conversion == 'B') value = numberValue(kBlockSize);
		else if (conversion == 'o') value = numberValue(kIoBlock);
		else if (conversion == 'h') value = numberValue(static_cast<long long>(info.links));
		else if (conversion == 'i') value = numberValue(static_cast<long long>(info.inode));
		else if (conversion == 'd') value = numberValue(device);
		else if (conversion == 'D') value = numberValue(device, 'x');
		else if (conversion == 't' || conversion == 'T') value = numberValue(0, 'x');
		else return false;

		return true;
	}

	static bool ownerDirective(char conversion, const Info& info, Value& value) {
		if (conversion == 'u' || conversion == 'g')      value = numberValue(kUserId);
		else if (conversion == 'U' || conversion == 'G') value = textValue(info.user);
		else if (conversion == 'a') value = numberValue(modeBits(info), 'o');
		else if (conversion == 'A') value = textValue(permissionText(info));
		else if (conversion == 'f') value = numberValue(typeBits(info) | modeBits(info), 'x');
		else if (conversion == 'F') value = textValue(fileTypeText(info));
		else if (conversion == 'n') value = textValue(info.name);
		else if (conversion == 'N') value = textValue(quotedName(info));
		else return false;

		return true;
	}

	static std::string padded(const Directive& directive, std::string text) {
		const std::size_t precision = static_cast<std::size_t>(directive.precision);
		if (directive.precision >= 0 && text.size() > precision) text.resize(precision);

		std::size_t width = 0;
		for (const char c : directive.width) width = width * 10 + static_cast<std::size_t>(c - '0');
		if (text.size() >= width) return text;

		const std::string fill(width - text.size(), ' ');
		const bool left = directive.flags.find('-') != std::string::npos;
		return left ? text + fill : fill + text;
	}

	static std::string formatted(const Directive& directive, const Value& value) {
		if (!value.numeric) return padded(directive, value.text);

		const std::string spec = "%" + directive.flags + directive.width + "ll" + value.base;
		char text[64];
		std::snprintf(text, sizeof(text), spec.c_str(), value.number);
		return text;
	}

	static bool parseDirective(const std::string& format, std::size_t& i, Directive& directive) {
		++i;
		while (i < format.size() && std::string("-#0+ '").find(format[i]) != std::string::npos) {
			directive.flags.push_back(format[i++]);
		}

		while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) {
			directive.width.push_back(format[i++]);
		}

		if (i < format.size() && format[i] == '.') {
			directive.precision = 0;
			while (++i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) {
				directive.precision = directive.precision * 10 + (format[i] - '0');
			}
		}

		if (i >= format.size()) return false;
		directive.conversion = format[i];
		return true;
	}

	struct Context {
		Executor& exec;
		const Info& info;
	};

	// An unknown directive prints `?`, as GNU stat does.
	static std::string expandDirective(const Context& context, const Directive& directive) {
		const char conversion = directive.conversion;
		if (conversion == '%') return "%";
		if (conversion == 'm') {
			return padded(directive, mountPoint(context.exec, context.info.name));
		}

		Value value;
		if (timeDirective(conversion, context.info, directive.precision, value)) {
			Directive whole_value = directive;
			whole_value.precision = -1;
			return formatted(whole_value, value);
		}

		const bool known = sizeDirective(conversion, context.info, value)
			|| ownerDirective(conversion, context.info, value);
		return known ? formatted(directive, value) : "?";
	}

	static std::string expandFormat(const Context& context, const std::string& format) {
		std::string out;
		for (std::size_t i = 0; i < format.size(); ++i) {
			if (format[i] != '%') {
				out.push_back(format[i]);
				continue;
			}

			const std::size_t start = i;
			Directive directive;
			if (!parseDirective(format, i, directive)) {
				out.append(format, start, std::string::npos);
				break;
			}

			out += expandDirective(context, directive);
		}

		return out;
	}

	static std::string escapedPercents(const std::string& text) {
		std::string out;
		for (const char c : text) {
			if (c == '%') out.push_back('%');
			out.push_back(c);
		}

		return out;
	}

	// The default listing names the file unquoted, a link with its target.
	static std::string fileLine(const Info& info) {
		std::string name = info.name;
		if (info.is_link) name += " -> " + info.link_target;
		return "  File: " + escapedPercents(name) + "\n";
	}

	static std::string chosenFormat(const Options& options, const Info& info) {
		if (options.has_format) return options.format;
		if (options.terse) return kTerseFormat;
		return fileLine(info) + kDefaultFormat;
	}

	static char simpleEscape(char c) {
		switch (c) {
			case 'n':
				return '\n';
			case 't':
				return '\t';
			case 'r':
				return '\r';
			case 'a':
				return '\a';
			case 'b':
				return '\b';
			case 'f':
				return '\f';
			case 'v':
				return '\v';
			default:
				return c;
		}
	}

	static char octalEscape(const std::string& text, std::size_t& i) {
		int value = 0;
		for (int digits = 0; digits < 3 && i + 1 < text.size(); ++digits) {
			const char next = text[i + 1];
			if (next < '0' || next > '7') break;

			value = value * 8 + (next - '0');
			++i;
		}

		return static_cast<char>(value);
	}

	// --printf reads backslash escapes like printf(1).
	static std::string withEscapes(const std::string& text, bool interpret) {
		if (!interpret) return text;

		std::string out;
		for (std::size_t i = 0; i < text.size(); ++i) {
			if (text[i] != '\\' || i + 1 >= text.size()) {
				out.push_back(text[i]);
				continue;
			}

			const char next = text[i + 1];
			if (next >= '0' && next <= '7') {
				out.push_back(octalEscape(text, i));
				continue;
			}

			out.push_back(simpleEscape(next));
			++i;
		}

		return out;
	}

	static bool statOne(Executor& exec, const std::string& path, const Options& options) {
		Info info;
		if (!collectInfo(exec, path, options.dereference, info)) {
			perr("stat", "cannot stat '" + path + "': No such file or directory");
			return false;
		}

		const std::string chosen = chosenFormat(options, info);
		const std::string format = withEscapes(chosen, options.interpret_escapes);
		const std::string text = expandFormat({ exec, info }, format);
		std::fwrite(text.data(), 1, text.size(), stdout);
		return true;
	}

	static int builtin_stat(Executor& exec, const std::vector<std::string>& args) {
		Options options;
		if (!parseOptions(args, options)) return 1;

		int status = 0;
		for (const std::string& path : options.files) {
			if (!statOne(exec, path, options)) status = 1;
		}

		std::fflush(stdout);
		return status;
	}

}  // namespace wbsh::stat_detail

namespace wbsh {

	void registerStatBuiltins(Executor& exec) {
		exec.registerBuiltin("touch", touch_detail::builtin_touch);
		exec.registerBuiltin("stat",  stat_detail::builtin_stat);
	}

}  // namespace wbsh
