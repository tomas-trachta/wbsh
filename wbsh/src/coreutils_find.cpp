/**
 * @file coreutils_find.cpp
 * @brief find with the GNU findutils expression language.
 *
 * `find [-H|-L|-P] [path...] [expression]`. The expression is parsed into a
 * tree of tests, actions and positional options joined by `!` / -not,
 * -a / -and (or plain juxtaposition), -o / -or, `,` and parentheses, then
 * evaluated for every file of a depth-first walk. Links are followed only
 * with -L (or -H for the starting points); a followed link that leads back
 * to an ancestor directory is reported as a file system loop. Owners,
 * groups and permission bits are the same synthetic values `ls -l` shows.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>

#  include <io.h>
#endif /* _WIN32 */

#include "coreutils_internal.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <regex>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "fnmatch.h"
#include "numparse.h"
#include "pathconv.h"
#include "posixregex.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace find_detail {

		static const int           kOwnerId          = 1000;
		static const std::int64_t  kTicksPerSecond   = 10000000;
		static const std::int64_t  kUnixEpochTicks   = 116444736000000000LL;
		static const double        kSecondsPerMinute = 60.0;
		static const double        kSecondsPerDay    = 86400.0;
		static const double        kSixMonths        = 182.5 * 86400.0;
		static const std::size_t   kMaxBatchChars    = 30000;
		static const int           kCommandNotFound  = 127;
		static const std::size_t   kFieldCapacity    = 64;

		enum class Follow { Never, CommandLine, Always };

		enum class RegexType { Emacs, Basic, Extended };

		enum class Op { And, Or, Not, Comma, Test };

		enum class Compare { Less, Equal, Greater };

		enum class TimeField { Access, Birth, Change, Modify };

		enum class PermMatch { Exact, All, Any };

		struct Number {
			Compare compare = Compare::Equal;
			long long value = 0;
		};

		struct FileStat {
			bool ok = false;
			std::uint64_t size = 0;
			std::int64_t atime = 0;
			std::int64_t btime = 0;
			std::int64_t ctime = 0;
			std::int64_t mtime = 0;
			std::uint32_t links = 1;
			std::uint32_t attributes = 0;
			FileIdentity identity;
		};

		struct Entry {
			std::string path;
			std::string root;
			std::string name;
			fs::path native;
			int depth = 0;
			bool follow = false;
			bool pruned = false;
			bool have_type = false;
			char type = 'U';
			bool have_stat = false;
			FileStat stat;
		};

		struct Run;
		struct Node;

		using TestFn = bool (*)(Run&, Node&, Entry&);

		struct Node {
			Op op = Op::Test;
			int left = -1;
			int right = -1;
			TestFn test = nullptr;
			std::string text;
			Number number;
			double unit = 1.0;
			TimeField field = TimeField::Modify;
			std::int64_t reference = 0;
			FileIdentity identity;
			unsigned mode = 0;
			PermMatch perm = PermMatch::Exact;
			std::regex regex;
			std::FILE* out = stdout;
			char terminator = '\n';
			std::vector<std::string> command;
			bool batch = false;
			bool in_dir = false;
			bool ask = false;
			std::vector<std::string> pending;
			std::string pending_dir;
			std::size_t pending_chars = 0;
		};

		struct Options {
			Follow follow = Follow::Never;
			RegexType regex_type = RegexType::Emacs;
			int max_depth = -1;
			int min_depth = 0;
			bool depth_first = false;
			bool same_device = false;
			bool day_start = false;
			std::vector<std::string> roots;
		};

		struct Ancestor {
			FileIdentity identity;
			std::string path;
		};

		struct Run {
			Executor* exec = nullptr;
			Options options;
			std::vector<Node> nodes;
			int expression = -1;
			std::int64_t now = 0;
			bool quit = false;
			int status = 0;
			std::uint64_t root_volume = 0;
			std::vector<Ancestor> ancestors;
			std::vector<std::FILE*> outputs;
		};

		struct Parser {
			const std::vector<std::string>* args = nullptr;
			std::size_t pos = 0;
			Run* run = nullptr;
			bool ok = true;
			bool has_action = false;
			bool has_delete = false;
			bool has_prune = false;
			bool explicit_depth = false;
		};

		static void reportFindError(Run& run, const std::string& path, const std::string& reason) {
			std::fflush(stdout);
			std::fprintf(stderr, "find: '%s': %s\n", path.c_str(), reason.c_str());
			run.status = 1;
		}

		static bool isSlash(char c) {
			return c == '/' || c == '\\';
		}

		static std::string baseName(const std::string& path) {
			std::size_t end = path.size();
			while (end > 1 && isSlash(path[end - 1])) --end;

			const std::size_t slash = path.find_last_of("/\\", end - 1);
			if (slash == std::string::npos || slash + 1 == end) return path.substr(0, end);
			return path.substr(slash + 1, end - slash - 1);
		}

		static std::string dirName(const std::string& path) {
			const std::size_t slash = path.find_last_of("/\\");
			if (slash == std::string::npos) return ".";
			if (slash == 0) return "/";
			return path.substr(0, slash);
		}

		static std::string lowerAscii(std::string text) {
			for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return text;
		}

		static bool isAllDigits(const std::string& text) {
			if (text.empty()) return false;
			for (char c : text) {
				if (!std::isdigit(static_cast<unsigned char>(c))) return false;
			}

			return true;
		}

		static std::int64_t unixTicks(std::uint32_t high, std::uint32_t low) {
			const std::uint64_t ticks = (static_cast<std::uint64_t>(high) << 32) | low;
			return static_cast<std::int64_t>(ticks) - kUnixEpochTicks;
		}

		static std::int64_t unixTicks(const FILETIME& time) {
			return unixTicks(time.dwHighDateTime, time.dwLowDateTime);
		}

		static std::int64_t unixTicks(const LARGE_INTEGER& time) {
			return time.QuadPart - kUnixEpochTicks;
		}

		static void fillFromHandleInfo(const BY_HANDLE_FILE_INFORMATION& info, FileStat& stat) {
			stat.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
			stat.atime = unixTicks(info.ftLastAccessTime);
			stat.btime = unixTicks(info.ftCreationTime);
			stat.mtime = unixTicks(info.ftLastWriteTime);
			stat.ctime = stat.mtime;
			stat.links = info.nNumberOfLinks;
			stat.attributes = info.dwFileAttributes;
			stat.identity.volume = info.dwVolumeSerialNumber;
			stat.identity.index = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32)
				| info.nFileIndexLow;
			stat.ok = true;
		}

		static bool statByHandle(const fs::path& native, bool follow, FileStat& stat) {
			const DWORD reparse = follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT;
			const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | reparse;
			HANDLE handle = ::CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				flags, nullptr);
			if (handle == INVALID_HANDLE_VALUE) return false;

			BY_HANDLE_FILE_INFORMATION info{};
			FILE_BASIC_INFO basic{};
			const bool ok = ::GetFileInformationByHandle(handle, &info) != FALSE;
			const bool have_basic = ok && ::GetFileInformationByHandleEx(handle, FileBasicInfo,
				&basic, sizeof(basic)) != FALSE;
			::CloseHandle(handle);
			if (!ok) return false;

			fillFromHandleInfo(info, stat);
			if (have_basic) stat.ctime = unixTicks(basic.ChangeTime);
			return true;
		}

		static bool statByAttributes(const fs::path& native, FileStat& stat) {
			WIN32_FILE_ATTRIBUTE_DATA data{};
			if (!::GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data)) return false;

			stat.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			stat.atime = unixTicks(data.ftLastAccessTime);
			stat.btime = unixTicks(data.ftCreationTime);
			stat.mtime = unixTicks(data.ftLastWriteTime);
			stat.ctime = stat.mtime;
			stat.attributes = data.dwFileAttributes;
			stat.ok = true;
			return true;
		}

		static const FileStat& entryStat(Entry& entry) {
			if (entry.have_stat) return entry.stat;

			entry.have_stat = true;
			if (!statByHandle(entry.native, entry.follow, entry.stat)) {
				statByAttributes(entry.native, entry.stat);
			}

			return entry.stat;
		}

		static std::int64_t statTime(const FileStat& stat, TimeField field) {
			switch (field) {
				case TimeField::Access: return stat.atime;
				case TimeField::Birth:  return stat.btime;
				case TimeField::Change: return stat.ctime;
				case TimeField::Modify: return stat.mtime;
				default:                return stat.mtime;
			}
		}

		static char typeLetter(fs::file_type type) {
			switch (type) {
				case fs::file_type::regular:   return 'f';
				case fs::file_type::directory: return 'd';
				case fs::file_type::symlink:   return 'l';
				case fs::file_type::junction:  return 'l';
				case fs::file_type::block:     return 'b';
				case fs::file_type::character: return 'c';
				case fs::file_type::fifo:      return 'p';
				case fs::file_type::socket:    return 's';
				case fs::file_type::not_found: return 'N';
				default:                       return 'U';
			}
		}

		static char linkType(const Entry& entry) {
			std::error_code ec;
			return typeLetter(fs::symlink_status(entry.native, ec).type());
		}

		static char targetType(const Entry& entry) {
			std::error_code ec;
			const fs::file_status target = fs::status(entry.native, ec);
			return ec ? 'N' : typeLetter(target.type());
		}

		static char entryType(Entry& entry) {
			if (entry.have_type) return entry.type;

			char type = linkType(entry);
			if (entry.follow && type == 'l') {
				const char target = targetType(entry);
				if (target != 'N') type = target;
			}

			entry.type = type;
			entry.have_type = true;
			return type;
		}

		static std::string linkTarget(const Entry& entry) {
			if (linkType(entry) != 'l') return std::string();

			std::error_code ec;
			std::string target = pathToUtf8(fs::read_symlink(entry.native, ec));
			for (char& c : target) {
				if (c == '\\') c = '/';
			}

			return target;
		}

		static unsigned entryMode(Entry& entry) {
			const char type = entryType(entry);
			if (type == 'l') return 0777;

			unsigned mode = 0644;
			if (type == 'd' || hasExecutableExtension(entry.native)) mode = 0755;

			const FileStat& stat = entryStat(entry);
			if (stat.ok && (stat.attributes & FILE_ATTRIBUTE_READONLY) != 0) mode &= ~0222u;
			return mode;
		}

		static std::string modeString(Entry& entry) {
			static const char kLetters[] = "rwxrwxrwx";
			const unsigned mode = entryMode(entry);
			const char type = entryType(entry);

			std::string text(1, type == 'f' ? '-' : type);
			for (int bit = 0; bit < 9; ++bit) {
				text.push_back((mode & (0400u >> bit)) != 0 ? kLetters[bit] : '-');
			}

			return text;
		}

		static std::int64_t currentTicks() {
			FILETIME now{};
			::GetSystemTimeAsFileTime(&now);
			return unixTicks(now);
		}

		static std::int64_t startOfTomorrowTicks() {
			const std::time_t now = std::time(nullptr);
			std::tm local{};
			localtime_s(&local, &now);
			local.tm_hour = 0;
			local.tm_min = 0;
			local.tm_sec = 0;
			local.tm_mday += 1;
			local.tm_isdst = -1;
			return static_cast<std::int64_t>(std::mktime(&local)) * kTicksPerSecond;
		}

		static bool parserFail(Parser& p, const std::string& message) {
			if (p.ok) std::fprintf(stderr, "find: %s\n", message.c_str());
			p.ok = false;
			return false;
		}

		static bool takeArgument(Parser& p, const std::string& primary, std::string& value) {
			if (p.pos >= p.args->size()) {
				return parserFail(p, "missing argument to `" + primary + "'");
			}

			value = (*p.args)[p.pos++];
			return true;
		}

		static bool invalidArgument(Parser& p, const std::string& value,
		                            const std::string& primary) {
			return parserFail(p, "invalid argument `" + value + "' to `" + primary + "'");
		}

		static std::size_t parseSign(const std::string& text, Number& number) {
			if (text.empty()) return 0;
			if (text[0] == '+') number.compare = Compare::Greater;
			if (text[0] == '-') number.compare = Compare::Less;
			return number.compare == Compare::Equal ? 0 : 1;
		}

		static bool parseNumberPrefix(const std::string& text, Number& number,
		                              std::string& suffix) {
			number = Number();
			const std::size_t start = parseSign(text, number);
			std::size_t end = start;
			while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
			if (end == start) return false;

			suffix = text.substr(end);
			return parseLL(text.substr(start, end - start), number.value);
		}

		static bool compareNumber(const Number& number, double value) {
			const double wanted = static_cast<double>(number.value);
			switch (number.compare) {
				case Compare::Less:    return value < wanted;
				case Compare::Greater: return value > wanted;
				case Compare::Equal:   return value == wanted;
				default:               return false;
			}
		}

		static bool readExactNumber(Parser& p, const std::string& name, Number& number) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			std::string suffix;
			if (!parseNumberPrefix(value, number, suffix) || !suffix.empty()) {
				return invalidArgument(p, value, name);
			}

			return true;
		}

		static bool testTrue(Run&, Node&, Entry&) {
			return true;
		}

		static bool testFalse(Run&, Node&, Entry&) {
			return false;
		}

		static bool testName(Run&, Node& node, Entry& entry) {
			return fnmatchFull(node.text, entry.name);
		}

		static bool testIName(Run&, Node& node, Entry& entry) {
			return fnmatchFull(node.text, lowerAscii(entry.name));
		}

		static bool testPath(Run&, Node& node, Entry& entry) {
			return fnmatchFull(node.text, entry.path);
		}

		static bool testIPath(Run&, Node& node, Entry& entry) {
			return fnmatchFull(node.text, lowerAscii(entry.path));
		}

		static bool testLinkName(Run&, Node& node, Entry& entry) {
			if (linkType(entry) != 'l') return false;
			return fnmatchFull(node.text, linkTarget(entry));
		}

		static bool testILinkName(Run&, Node& node, Entry& entry) {
			if (linkType(entry) != 'l') return false;
			return fnmatchFull(node.text, lowerAscii(linkTarget(entry)));
		}

		static bool testRegex(Run&, Node& node, Entry& entry) {
			return matchRegex(entry.path, node.regex);
		}

		static bool testType(Run&, Node& node, Entry& entry) {
			return node.text.find(entryType(entry)) != std::string::npos;
		}

		static bool testXType(Run&, Node& node, Entry& entry) {
			char type = linkType(entry);
			if (!entry.follow && type == 'l') {
				const char target = targetType(entry);
				if (target != 'N') type = target;
			}

			return node.text.find(type) != std::string::npos;
		}

		static bool isEmptyDirectory(const fs::path& native) {
			std::error_code ec;
			fs::directory_iterator it(native, ec);
			return !ec && it == fs::directory_iterator();
		}

		static bool testEmpty(Run&, Node&, Entry& entry) {
			const char type = entryType(entry);
			if (type == 'd') return isEmptyDirectory(entry.native);
			if (type != 'f') return false;

			const FileStat& stat = entryStat(entry);
			return stat.ok && stat.size == 0;
		}

		static bool testSize(Run&, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			if (!stat.ok) return false;

			const double units = std::ceil(static_cast<double>(stat.size) / node.unit);
			return compareNumber(node.number, units);
		}

		static bool testAge(Run& run, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			if (!stat.ok) return false;

			const std::int64_t ticks = run.now - statTime(stat, node.field);
			const double seconds = static_cast<double>(ticks) / kTicksPerSecond;
			return compareNumber(node.number, std::floor(seconds / node.unit));
		}

		static bool testNewer(Run&, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			return stat.ok && statTime(stat, node.field) > node.reference;
		}

		static bool testLinks(Run&, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			return stat.ok && compareNumber(node.number, static_cast<double>(stat.links));
		}

		static bool testInum(Run&, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			return stat.ok && compareNumber(node.number, static_cast<double>(stat.identity.index));
		}

		static bool testSameFile(Run&, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			return stat.ok && stat.identity == node.identity;
		}

		static bool testPerm(Run&, Node& node, Entry& entry) {
			const unsigned mode = entryMode(entry);
			switch (node.perm) {
				case PermMatch::All:   return (mode & node.mode) == node.mode;
				case PermMatch::Any:   return node.mode == 0 || (mode & node.mode) != 0;
				case PermMatch::Exact: return mode == node.mode;
				default:               return false;
			}
		}

		static bool testReadable(Run&, Node&, Entry& entry) {
			return ::_waccess(entry.native.c_str(), 4) == 0;
		}

		static bool testWritable(Run&, Node&, Entry& entry) {
			return ::_waccess(entry.native.c_str(), 2) == 0;
		}

		static bool testExecutable(Run&, Node&, Entry& entry) {
			return (entryMode(entry) & 0111u) != 0;
		}

		static bool testPrune(Run& run, Node&, Entry& entry) {
			if (!run.options.depth_first) entry.pruned = true;
			return true;
		}

		static bool testQuit(Run& run, Node&, Entry&) {
			run.quit = true;
			return true;
		}

		static bool testPrint(Run&, Node& node, Entry& entry) {
			std::fwrite(entry.path.data(), 1, entry.path.size(), node.out);
			std::fputc(node.terminator, node.out);
			return true;
		}

		static bool testDelete(Run& run, Node&, Entry& entry) {
			if (entry.path == ".") return true;

			std::error_code ec;
			if (removeFileOrEmptyDirectory(entry.native, ec)) return true;

			std::fflush(stdout);
			std::fprintf(stderr, "find: cannot delete '%s': %s\n", entry.path.c_str(),
				posixErrorText(ec).c_str());
			run.status = 1;
			return false;
		}

		static std::tm localTime(std::int64_t ticks) {
			const std::time_t seconds = static_cast<std::time_t>(ticks / kTicksPerSecond);
			std::tm local{};
			localtime_s(&local, &seconds);
			return local;
		}

		static std::string strftimeText(const std::tm& time, const char* format) {
			char text[kFieldCapacity * 2];
			const std::size_t length = std::strftime(text, sizeof(text), format, &time);
			return std::string(text, length);
		}

		static std::string fractionText(std::int64_t ticks) {
			const long long nanoseconds = (ticks % kTicksPerSecond) * 100;
			char text[kFieldCapacity];
			std::snprintf(text, sizeof(text), "%09lld0", nanoseconds);
			return text;
		}

		static std::string ctimeText(std::int64_t ticks) {
			const std::tm local = localTime(ticks);
			return strftimeText(local, "%a %b %e %H:%M:%S.") + fractionText(ticks)
				+ strftimeText(local, " %Y");
		}

		static std::string timeDirective(std::int64_t ticks, char key) {
			const std::tm local = localTime(ticks);
			const long long seconds = ticks / kTicksPerSecond;
			if (key == '@') return std::to_string(seconds) + "." + fractionText(ticks);
			if (key == '+') return strftimeText(local, "%Y-%m-%d+%H:%M:%S.") + fractionText(ticks);
			if (key == 'S') return strftimeText(local, "%S.") + fractionText(ticks);
			if (key == 'T') return strftimeText(local, "%H:%M:%S.") + fractionText(ticks);

			const char format[] = { '%', key, '\0' };
			return strftimeText(local, format);
		}

		static TimeField timeFieldForDirective(char directive) {
			if (directive == 'A' || directive == 'a') return TimeField::Access;
			if (directive == 'B' || directive == 'b') return TimeField::Birth;
			if (directive == 'C' || directive == 'c') return TimeField::Change;
			return TimeField::Modify;
		}

		static std::string relativeToRoot(const Entry& entry) {
			std::string rest = entry.path.substr(std::min(entry.root.size(), entry.path.size()));
			while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
			return rest;
		}

		static std::string octalText(unsigned value) {
			char text[kFieldCapacity];
			std::snprintf(text, sizeof(text), "%o", value);
			return text;
		}

		static std::string blocksText(const FileStat& stat, std::uint64_t unit) {
			return std::to_string((stat.size + unit - 1) / unit);
		}

		static bool statDirective(Entry& entry, char directive, std::string& value) {
			const FileStat& stat = entryStat(entry);
			switch (directive) {
				case 's': value = std::to_string(stat.size);              return true;
				case 'n': value = std::to_string(stat.links);             return true;
				case 'i': value = std::to_string(stat.identity.index);    return true;
				case 'D': value = std::to_string(stat.identity.volume);   return true;
				case 'k': value = blocksText(stat, 1024);                 return true;
				case 'b': value = blocksText(stat, 512);                  return true;
				case 'a': value = ctimeText(stat.atime);                  return true;
				case 'c': value = ctimeText(stat.ctime);                  return true;
				case 't': value = ctimeText(stat.mtime);                  return true;
				default:  return false;
			}
		}

		static bool nameDirective(Run& run, Entry& entry, char directive, std::string& value) {
			switch (directive) {
				case 'p': value = entry.path;                                  return true;
				case 'f': value = entry.name;                                  return true;
				case 'h': value = dirName(entry.path);                         return true;
				case 'P': value = relativeToRoot(entry);                       return true;
				case 'H': value = entry.root;                                  return true;
				case 'd': value = std::to_string(entry.depth);                 return true;
				case 'y': value = std::string(1, entryType(entry));            return true;
				case 'Y': value = std::string(1, targetType(entry));           return true;
				case 'l': value = linkTarget(entry);                           return true;
				case 'm': value = octalText(entryMode(entry));                 return true;
				case 'M': value = modeString(entry);                           return true;
				case 'u': value = currentUserName(*run.exec);                  return true;
				case 'g': value = currentUserName(*run.exec);                  return true;
				case 'U': value = std::to_string(kOwnerId);       return true;
				case 'G': value = std::to_string(kOwnerId);       return true;
				default:  return statDirective(entry, directive, value);
			}
		}

		static std::string padField(const std::string& spec, const std::string& value) {
			if (spec.empty()) return value;

			std::string format = "%";
			for (char c : spec) {
				if (c != '#' && c != '+' && c != ' ' && c != '0') format.push_back(c);
			}

			format.push_back('s');
			const int length = std::snprintf(nullptr, 0, format.c_str(), value.c_str());
			if (length <= 0) return value;

			std::string out(static_cast<std::size_t>(length) + 1, '\0');
			std::snprintf(&out[0], out.size(), format.c_str(), value.c_str());
			out.resize(static_cast<std::size_t>(length));
			return out;
		}

		static std::size_t skipFieldSpec(const std::string& format, std::size_t i) {
			while (i < format.size() && std::strchr("-+ #0", format[i]) != nullptr) ++i;
			while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) ++i;
			if (i < format.size() && format[i] == '.') ++i;
			while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) ++i;
			return i;
		}

		static bool timeKeyDirective(Entry& entry, char directive, char key, std::string& value) {
			const FileStat& stat = entryStat(entry);
			value = timeDirective(statTime(stat, timeFieldForDirective(directive)), key);
			return true;
		}

		static void appendDirective(Run& run, const std::string& format, std::size_t& i,
		                            Entry& entry, std::string& out) {
			const std::size_t start = i;
			i = skipFieldSpec(format, i);
			const std::string spec = format.substr(start, i - start);
			if (i >= format.size()) {
				out += "%" + spec;
				return;
			}

			const char directive = format[i++];
			const bool timed = std::strchr("ABCT", directive) != nullptr && i < format.size();
			std::string value;
			bool known = false;
			if (directive == '%') {
				value = "%";
				known = true;
			} else if (timed) {
				known = timeKeyDirective(entry, directive, format[i++], value);
			} else {
				known = nameDirective(run, entry, directive, value);
			}

			if (!known) {
				out += "%" + spec + directive;
				return;
			}

			out += padField(spec, value);
		}

		static char simpleEscape(char c) {
			switch (c) {
				case 'a':  return '\a';
				case 'b':  return '\b';
				case 'f':  return '\f';
				case 'n':  return '\n';
				case 'r':  return '\r';
				case 't':  return '\t';
				case 'v':  return '\v';
				case '\\': return '\\';
				default:   return '\0';
			}
		}

		static void appendOctalEscape(const std::string& format, std::size_t& i, std::string& out) {
			unsigned value = 0;
			for (int digits = 0; digits < 3 && i < format.size(); ++digits) {
				const char c = format[i];
				if (c < '0' || c > '7') break;
				value = value * 8 + static_cast<unsigned>(c - '0');
				++i;
			}

			out.push_back(static_cast<char>(value & 0xFF));
		}

		static bool appendEscape(const std::string& format, std::size_t& i, std::string& out) {
			if (i >= format.size()) {
				out.push_back('\\');
				return true;
			}

			const char c = format[i];
			if (c == 'c') return false;
			if (c >= '0' && c <= '7') {
				appendOctalEscape(format, i, out);
				return true;
			}

			++i;
			const char escaped = simpleEscape(c);
			if (escaped != '\0') {
				out.push_back(escaped);
				return true;
			}

			out.push_back('\\');
			out.push_back(c);
			return true;
		}

		static std::string expandFormat(Run& run, const std::string& format, Entry& entry) {
			std::string out;
			std::size_t i = 0;
			while (i < format.size()) {
				const char c = format[i++];
				if (c == '\\') {
					if (!appendEscape(format, i, out)) break;
					continue;
				}

				if (c == '%') {
					appendDirective(run, format, i, entry, out);
					continue;
				}

				out.push_back(c);
			}

			return out;
		}

		static bool testPrintf(Run& run, Node& node, Entry& entry) {
			const std::string text = expandFormat(run, node.text, entry);
			std::fwrite(text.data(), 1, text.size(), node.out);
			return true;
		}

		static std::string lsTime(const Run& run, std::int64_t ticks) {
			const double age = static_cast<double>(run.now - ticks) / kTicksPerSecond;
			const bool recent = age >= -kSixMonths && age <= kSixMonths;
			return strftimeText(localTime(ticks), recent ? "%b %e %H:%M" : "%b %e  %Y");
		}

		static bool testLs(Run& run, Node& node, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			const std::string user = currentUserName(*run.exec);
			std::fprintf(node.out, "%9llu %6llu %s %3u %-8s %-8s %8llu %s %s",
				static_cast<unsigned long long>(stat.identity.index),
				static_cast<unsigned long long>((stat.size + 1023) / 1024),
				modeString(entry).c_str(), stat.links, user.c_str(), user.c_str(),
				static_cast<unsigned long long>(stat.size), lsTime(run, stat.mtime).c_str(),
				entry.path.c_str());

			const std::string target = linkTarget(entry);
			if (!target.empty()) std::fprintf(node.out, " -> %s", target.c_str());
			std::fputc('\n', node.out);
			return true;
		}

		static std::string replaceBraces(const std::string& word, const std::string& target) {
			std::string out;
			std::size_t pos = 0;
			for (std::size_t hit = word.find("{}"); hit != std::string::npos;
			     hit = word.find("{}", pos)) {
				out.append(word, pos, hit - pos);
				out += target;
				pos = hit + 2;
			}

			out.append(word, pos, std::string::npos);
			return out;
		}

		static std::string execTarget(const Entry& entry, bool in_dir) {
			return in_dir ? "./" + entry.name : entry.path;
		}

		static std::string execDirectory(const Entry& entry, bool in_dir) {
			if (!in_dir) return std::string();
			if (entry.depth == 0 && entry.name == entry.path) return ".";
			return dirName(entry.path);
		}

		static bool commandExists(Executor& exec, const std::string& name, bool in_dir) {
			if (in_dir && name.find('/') != std::string::npos) return true;
			if (exec.isBuiltin(name) || exec.isFunction(name)) return true;
			return !exec.findExecutable(name).empty();
		}

		static int runCommand(Run& run, const std::vector<std::string>& argv,
		                      const std::string& directory) {
			std::fflush(stdout);
			if (!commandExists(*run.exec, argv[0], !directory.empty())) {
				std::fprintf(stderr, "find: '%s': No such file or directory\n", argv[0].c_str());
				return kCommandNotFound;
			}

			std::string text = quoteShellArgv(argv);
			if (!directory.empty()) {
				text = "( cd -- " + quoteShellArgv({ directory }) + " && " + text + " )";
			}

			const int status = run.exec->executeText(text, "<find>");
			std::fflush(stdout);
			return status;
		}

		static bool confirmCommand(const std::string& command, const std::string& target) {
			std::fflush(stdout);
			std::fprintf(stderr, "< %s ... %s > ? ", command.c_str(), target.c_str());
			std::fflush(stderr);

			char answer[256] = {};
			if (std::fgets(answer, sizeof(answer), stdin) == nullptr) return false;
			return answer[0] == 'y' || answer[0] == 'Y';
		}

		static void flushBatch(Run& run, Node& node) {
			if (node.pending.empty()) return;

			std::vector<std::string> argv = node.command;
			argv.insert(argv.end(), node.pending.begin(), node.pending.end());
			if (runCommand(run, argv, node.pending_dir) != 0) run.status = 1;

			node.pending.clear();
			node.pending_chars = 0;
		}

		static bool queueBatch(Run& run, Node& node, Entry& entry) {
			const std::string directory = execDirectory(entry, node.in_dir);
			const std::string target = execTarget(entry, node.in_dir);
			const bool full = node.pending_chars + target.size() + 1 > kMaxBatchChars;
			const bool moved = directory != node.pending_dir;
			if (!node.pending.empty() && (full || moved)) flushBatch(run, node);

			node.pending_dir = directory;
			node.pending.push_back(target);
			node.pending_chars += target.size() + 1;
			return true;
		}

		static bool testExec(Run& run, Node& node, Entry& entry) {
			if (node.batch) return queueBatch(run, node, entry);

			const std::string target = execTarget(entry, node.in_dir);
			std::vector<std::string> argv;
			for (const std::string& word : node.command) {
				argv.push_back(replaceBraces(word, target));
			}

			if (node.ask && !confirmCommand(argv[0], target)) return false;

			return runCommand(run, argv, execDirectory(entry, node.in_dir)) == 0;
		}

		static void flushAllBatches(Run& run) {
			for (Node& node : run.nodes) {
				if (node.batch) flushBatch(run, node);
			}
		}

		static bool parseOptionFlag(Parser& p, const std::string& name, Node& node) {
			find_detail::Options& options = p.run->options;
			node.test = testTrue;
			if (name == "-depth" || name == "-d") {
				options.depth_first = true;
				p.explicit_depth = true;
			}

			if (name == "-xdev" || name == "-mount") options.same_device = true;
			if (name == "-follow") options.follow = Follow::Always;
			if (name == "-daystart") options.day_start = true;
			return true;
		}

		static bool parseDepthLimit(Parser& p, const std::string& name, Node& node) {
			node.test = testTrue;
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			int depth = 0;
			if (!isAllDigits(value) || !parseInt(value, depth)) {
				return parserFail(p, "Expected a positive decimal integer argument to " + name
					+ ", but got '" + value + "'");
			}

			if (name == "-maxdepth") p.run->options.max_depth = depth;
			else                     p.run->options.min_depth = depth;
			return true;
		}

		static bool regexTypeNamed(const std::string& value, RegexType& type) {
			struct Named { const char* name; RegexType type; };
			static const Named kTypes[] = {
				{ "findutils-default", RegexType::Emacs },  { "emacs", RegexType::Emacs },
				{ "posix-basic", RegexType::Basic },        { "ed", RegexType::Basic },
				{ "posix-minimal-basic", RegexType::Basic }, { "sed", RegexType::Basic },
				{ "grep", RegexType::Basic },               { "awk", RegexType::Extended },
				{ "posix-extended", RegexType::Extended },  { "egrep", RegexType::Extended },
				{ "posix-egrep", RegexType::Extended },     { "posix-awk", RegexType::Extended },
				{ "gnu-awk", RegexType::Extended },
			};

			for (const Named& named : kTypes) {
				if (value != named.name) continue;
				type = named.type;
				return true;
			}

			return false;
		}

		static bool parseRegexType(Parser& p, const std::string& name, Node& node) {
			node.test = testTrue;
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			if (regexTypeNamed(value, p.run->options.regex_type)) return true;

			return parserFail(p, "Unknown regular expression type '" + value + "'; valid types "
				"are 'findutils-default', 'ed', 'emacs', 'gnu-awk', 'grep', 'posix-awk', 'awk', "
				"'posix-basic', 'posix-egrep', 'egrep', 'posix-extended', 'posix-minimal-basic', "
				"'sed'.");
		}

		static bool parsePattern(Parser& p, const std::string& name, Node& node) {
			if (!takeArgument(p, name, node.text)) return false;

			const bool fold = name == "-iname" || name == "-ipath" || name == "-iwholename"
				|| name == "-ilname";
			if (fold) node.text = lowerAscii(node.text);

			if (name == "-name")        node.test = testName;
			else if (name == "-iname")  node.test = testIName;
			else if (name == "-lname")  node.test = testLinkName;
			else if (name == "-ilname") node.test = testILinkName;
			else                        node.test = fold ? testIPath : testPath;
			return true;
		}

		static std::size_t copyBracket(const std::string& pattern, std::size_t i,
		                               std::string& out) {
			const std::size_t start = i;
			++i;
			if (i < pattern.size() && pattern[i] == '^') ++i;
			if (i < pattern.size() && pattern[i] == ']') ++i;
			while (i < pattern.size() && pattern[i] != ']') ++i;
			if (i >= pattern.size()) {
				out += pattern.substr(start);
				return pattern.size();
			}

			out += pattern.substr(start, i - start + 1);
			return i + 1;
		}

		// Emacs syntax: `\( \) \|` group and alternate, `( ) | { }` are literal,
		// `+ ?` are operators, which is ERE once the two sets are swapped.
		static std::string emacsToExtended(const std::string& pattern) {
			std::string out;
			std::size_t i = 0;
			while (i < pattern.size()) {
				const char c = pattern[i];
				if (c == '[') {
					i = copyBracket(pattern, i, out);
					continue;
				}

				if (c == '\\' && i + 1 < pattern.size()) {
					const char next = pattern[i + 1];
					if (std::strchr("()|", next) == nullptr) out.push_back('\\');
					out.push_back(next);
					i += 2;
					continue;
				}

				if (std::strchr("()|{}", c) != nullptr) out.push_back('\\');
				out.push_back(c);
				++i;
			}

			return out;
		}

		static bool parseRegex(Parser& p, const std::string& name, Node& node) {
			std::string pattern;
			if (!takeArgument(p, name, pattern)) return false;

			const RegexType type = p.run->options.regex_type;
			const bool extended = type != RegexType::Basic;
			if (type == RegexType::Emacs) pattern = emacsToExtended(pattern);

			node.test = testRegex;
			if (compilePosixRegex(node.regex, pattern, extended, name == "-iregex")) return true;
			return parserFail(p, "invalid regular expression `" + pattern + "'");
		}

		static bool parseType(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			for (std::size_t i = 0; i < value.size(); i += 2) {
				const bool letter = std::strchr("bcdpflsD", value[i]) != nullptr;
				const bool separated = i + 1 == value.size() || value[i + 1] == ',';
				if (!letter || !separated) {
					return parserFail(p, "Unknown argument to " + name + ": " + value);
				}

				node.text.push_back(value[i]);
			}

			if (node.text.empty()) {
				return parserFail(p, "Arguments to " + name
					+ " should contain at least one letter");
			}

			node.test = name == "-type" ? testType : testXType;
			return true;
		}

		static bool sizeUnit(const std::string& suffix, double& unit) {
			static const char kUnits[] = "cwbkMGT";
			static const double kBytes[] = {
				1, 2, 512, 1024, 1048576, 1073741824, 1099511627776.0,
			};
			if (suffix.empty()) {
				unit = 512;
				return true;
			}

			const char* hit = suffix.size() == 1 ? std::strchr(kUnits, suffix[0]) : nullptr;
			if (hit == nullptr || *hit == '\0') return false;

			unit = kBytes[hit - kUnits];
			return true;
		}

		static bool parseSize(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			std::string suffix;
			if (!parseNumberPrefix(value, node.number, suffix)) {
				return invalidArgument(p, value, name);
			}

			if (!sizeUnit(suffix, node.unit)) {
				return parserFail(p, "invalid -size type `" + suffix + "'");
			}

			node.test = testSize;
			return true;
		}

		static TimeField timeFieldFromLetter(char letter) {
			switch (letter) {
				case 'a': return TimeField::Access;
				case 'B': return TimeField::Birth;
				case 'c': return TimeField::Change;
				default:  return TimeField::Modify;
			}
		}

		static bool parseAge(Parser& p, const std::string& name, Node& node) {
			if (!readExactNumber(p, name, node.number)) return false;

			node.field = timeFieldFromLetter(name[1]);
			const bool minutes = name.compare(2, 3, "min") == 0;
			node.unit = minutes ? kSecondsPerMinute : kSecondsPerDay;
			node.test = testAge;
			return true;
		}

		static bool referenceStat(Parser& p, const std::string& file, FileStat& stat) {
			if (statByHandle(toNative(*p.run->exec, file), true, stat)) return true;

			std::fprintf(stderr, "find: '%s': No such file or directory\n", file.c_str());
			p.ok = false;
			return false;
		}

		static bool parseDateTicks(const std::string& text, std::int64_t& ticks) {
			long long seconds = 0;
			if (!text.empty() && text[0] == '@') {
				if (!parseLL(text.substr(1), seconds)) return false;
				ticks = seconds * kTicksPerSecond;
				return true;
			}

			std::tm local{};
			const int fields = std::sscanf(text.c_str(), "%d-%d-%d%*[ T]%d:%d:%d", &local.tm_year,
				&local.tm_mon, &local.tm_mday, &local.tm_hour, &local.tm_min, &local.tm_sec);
			if (fields != 3 && fields != 5 && fields != 6) return false;

			local.tm_year -= 1900;
			local.tm_mon -= 1;
			local.tm_isdst = -1;
			const std::time_t epoch = std::mktime(&local);
			if (epoch == static_cast<std::time_t>(-1)) return false;

			ticks = static_cast<std::int64_t>(epoch) * kTicksPerSecond;
			return true;
		}

		static bool newerReference(Parser& p, char letter, const std::string& value, Node& node) {
			if (letter == 't') {
				if (parseDateTicks(value, node.reference)) return true;
				return parserFail(p, "I cannot figure out how to interpret `" + value
					+ "' as a date or time");
			}

			FileStat stat;
			if (!referenceStat(p, value, stat)) return false;

			node.reference = statTime(stat, timeFieldFromLetter(letter));
			return true;
		}

		static bool parseNewer(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			char mine = 'm';
			char theirs = 'm';
			if (name == "-anewer") mine = 'a';
			if (name == "-cnewer") mine = 'c';
			if (name.size() == 8) {
				mine = name[6];
				theirs = name[7];
			}

			const bool valid = std::strchr("aBcm", mine) != nullptr
				&& std::strchr("aBcmt", theirs) != nullptr;
			if (!valid) return parserFail(p, "invalid predicate `" + name + "'");

			node.field = timeFieldFromLetter(mine);
			node.test = testNewer;
			return newerReference(p, theirs, value, node);
		}

		static bool parseCount(Parser& p, const std::string& name, Node& node) {
			if (!readExactNumber(p, name, node.number)) return false;

			node.test = name == "-links" ? testLinks : testInum;
			return true;
		}

		static bool parseSameFile(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			FileStat stat;
			if (!referenceStat(p, value, stat)) return false;

			node.identity = stat.identity;
			node.test = testSameFile;
			return true;
		}

		static unsigned whoMask(char who) {
			switch (who) {
				case 'u': return 04700;
				case 'g': return 02070;
				case 'o': return 01007;
				case 'a': return 07777;
				default:  return 0;
			}
		}

		static unsigned permissionMask(char permission) {
			switch (permission) {
				case 'r': return 0444;
				case 'w': return 0222;
				case 'x': return 0111;
				case 'X': return 0111;
				case 's': return 06000;
				case 't': return 01000;
				default:  return 0;
			}
		}

		static void applySymbolicClause(char op, unsigned who, unsigned bits, unsigned& mode) {
			bits &= who;
			if (op == '+') mode |= bits;
			if (op == '-') mode &= ~bits;
			if (op == '=') mode = (mode & ~who) | bits;
		}

		static bool parseSymbolicClause(const std::string& text, std::size_t& i, unsigned& mode) {
			unsigned who = 0;
			while (i < text.size() && whoMask(text[i]) != 0) who |= whoMask(text[i++]);
			if (who == 0) who = 07777;
			if (i >= text.size() || std::strchr("+-=", text[i]) == nullptr) return false;

			const char op = text[i++];
			unsigned bits = 0;
			while (i < text.size() && permissionMask(text[i]) != 0) {
				bits |= permissionMask(text[i++]);
			}

			applySymbolicClause(op, who, bits, mode);
			return true;
		}

		static bool parseModeText(const std::string& text, unsigned& mode) {
			unsigned long value = 0;
			const bool digits_only = text.find_first_not_of("01234567") == std::string::npos;
			const bool octal = !text.empty() && digits_only;
			if (octal) {
				if (!parseUL(text, value, 8) || value > 07777) return false;
				mode = static_cast<unsigned>(value);
				return true;
			}

			mode = 0;
			std::size_t i = 0;
			while (i < text.size()) {
				if (!parseSymbolicClause(text, i, mode)) return false;
				if (i < text.size() && text[i++] != ',') return false;
			}

			return !text.empty();
		}

		static bool parsePerm(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			std::string text = value;
			if (!text.empty() && (text[0] == '-' || text[0] == '/' || text[0] == '+')) {
				node.perm = text[0] == '-' ? PermMatch::All : PermMatch::Any;
				text.erase(0, 1);
			}

			if (!parseModeText(text, node.mode)) {
				return parserFail(p, "invalid mode '" + value + "'");
			}

			node.test = testPerm;
			return true;
		}

		static bool parseOwner(Parser& p, const std::string& name, Node& node) {
			std::string value;
			if (!takeArgument(p, name, value)) return false;

			const bool numeric = isAllDigits(value);
			const bool mine = value == currentUserName(*p.run->exec)
				|| value == std::to_string(kOwnerId);
			if (!mine && !numeric) {
				const char* what = name == "-user" ? "user" : "group";
				return parserFail(p, "'" + value + "' is not the name of a known " + what);
			}

			node.test = mine ? testTrue : testFalse;
			return true;
		}

		static bool parseOwnerId(Parser& p, const std::string& name, Node& node) {
			if (!readExactNumber(p, name, node.number)) return false;

			const bool mine = compareNumber(node.number, kOwnerId);
			node.test = mine ? testTrue : testFalse;
			return true;
		}

		static bool parsePrintf(Parser& p, const std::string& name, Node& node) {
			if (!takeArgument(p, name, node.text)) return false;

			node.test = testPrintf;
			p.has_action = true;
			return true;
		}

		static bool openOutputFile(Parser& p, const std::string& name, Node& node) {
			std::string file;
			if (!takeArgument(p, name, file)) return false;

			node.out = fopenNative(*p.run->exec, file, "wb");
			if (node.out != nullptr) {
				p.run->outputs.push_back(node.out);
				return true;
			}

			std::fprintf(stderr, "find: '%s': %s\n", file.c_str(), std::strerror(errno));
			node.out = stdout;
			p.ok = false;
			return false;
		}

		static bool parseFileOutput(Parser& p, const std::string& name, Node& node) {
			if (!openOutputFile(p, name, node)) return false;

			p.has_action = true;
			if (name == "-fprintf") return parsePrintf(p, name, node);

			node.terminator = name == "-fprint0" ? '\0' : '\n';
			node.test = name == "-fls" ? testLs : testPrint;
			return true;
		}

		static bool finishCommand(Parser& p, const std::string& name, Node& node) {
			if (node.command.empty()) return parserFail(p, "missing argument to `" + name + "'");

			node.test = testExec;
			p.has_action = true;
			return true;
		}

		static bool parseCommand(Parser& p, const std::string& name, Node& node) {
			node.in_dir = name == "-execdir" || name == "-okdir";
			node.ask = name == "-ok" || name == "-okdir";
			const std::vector<std::string>& args = *p.args;
			while (p.pos < args.size()) {
				const std::string& arg = args[p.pos++];
				if (arg == ";") return finishCommand(p, name, node);

				const bool ends_batch = arg == "+" && !node.ask && !node.command.empty()
					&& node.command.back() == "{}";
				if (ends_batch) {
					node.command.pop_back();
					node.batch = true;
					return finishCommand(p, name, node);
				}

				node.command.push_back(arg);
			}

			return parserFail(p, "missing argument to `" + name + "'");
		}

		static bool parseSimple(Parser& p, const std::string& name, Node& node) {
			struct Simple { const char* name; TestFn test; bool action; };
			static const Simple kSimple[] = {
				{ "-true", testTrue, false },          { "-false", testFalse, false },
				{ "-nouser", testFalse, false },       { "-nogroup", testFalse, false },
				{ "-empty", testEmpty, false },        { "-readable", testReadable, false },
				{ "-writable", testWritable, false },  { "-executable", testExecutable, false },
				{ "-prune", testPrune, false },        { "-quit", testQuit, false },
				{ "-print", testPrint, true },         { "-print0", testPrint, true },
				{ "-ls", testLs, true },               { "-delete", testDelete, true },
			};

			for (const Simple& simple : kSimple) {
				if (name != simple.name) continue;
				node.test = simple.test;
				p.has_action = p.has_action || simple.action;
			}

			node.terminator = name == "-print0" ? '\0' : '\n';
			if (name == "-delete") p.has_delete = true;
			if (name == "-prune") p.has_prune = true;
			return node.test != nullptr;
		}

		using PrimaryParser = bool (*)(Parser&, const std::string&, Node&);

		struct PrimarySpec {
			const char* name;
			PrimaryParser parse;
		};

		static const PrimarySpec kPrimaries[] = {
			{ "-depth", parseOptionFlag },        { "-d", parseOptionFlag },
			{ "-xdev", parseOptionFlag },         { "-mount", parseOptionFlag },
			{ "-follow", parseOptionFlag },       { "-daystart", parseOptionFlag },
			{ "-noleaf", parseOptionFlag },       { "-ignore_readdir_race", parseOptionFlag },
			{ "-warn", parseOptionFlag },         { "-noignore_readdir_race", parseOptionFlag },
			{ "-nowarn", parseOptionFlag },       { "-maxdepth", parseDepthLimit },
			{ "-mindepth", parseDepthLimit },     { "-regextype", parseRegexType },
			{ "-name", parsePattern },            { "-iname", parsePattern },
			{ "-path", parsePattern },            { "-ipath", parsePattern },
			{ "-wholename", parsePattern },       { "-iwholename", parsePattern },
			{ "-lname", parsePattern },           { "-ilname", parsePattern },
			{ "-regex", parseRegex },             { "-iregex", parseRegex },
			{ "-type", parseType },               { "-xtype", parseType },
			{ "-size", parseSize },               { "-mtime", parseAge },
			{ "-atime", parseAge },               { "-ctime", parseAge },
			{ "-mmin", parseAge },                { "-amin", parseAge },
			{ "-cmin", parseAge },                { "-newer", parseNewer },
			{ "-anewer", parseNewer },            { "-cnewer", parseNewer },
			{ "-links", parseCount },             { "-inum", parseCount },
			{ "-samefile", parseSameFile },       { "-perm", parsePerm },
			{ "-user", parseOwner },              { "-group", parseOwner },
			{ "-uid", parseOwnerId },             { "-gid", parseOwnerId },
			{ "-printf", parsePrintf },           { "-fprint", parseFileOutput },
			{ "-fprint0", parseFileOutput },      { "-fprintf", parseFileOutput },
			{ "-fls", parseFileOutput },          { "-exec", parseCommand },
			{ "-execdir", parseCommand },         { "-ok", parseCommand },
			{ "-okdir", parseCommand },
		};

		static PrimaryParser findPrimary(const std::string& name) {
			for (const PrimarySpec& spec : kPrimaries) {
				if (name == spec.name) return spec.parse;
			}

			if (name.size() == 8 && name.compare(0, 6, "-newer") == 0) return parseNewer;
			return parseSimple;
		}

		static bool atEnd(const Parser& p) {
			return p.pos >= p.args->size();
		}

		static const std::string& peek(const Parser& p) {
			return (*p.args)[p.pos];
		}

		static int addNode(Parser& p, const Node& node) {
			p.run->nodes.push_back(node);
			return static_cast<int>(p.run->nodes.size()) - 1;
		}

		static int combine(Parser& p, Op op, int left, int right) {
			Node node;
			node.op = op;
			node.left = left;
			node.right = right;
			return addNode(p, node);
		}

		static bool isBinaryOperator(const std::string& arg) {
			return arg == "-o" || arg == "-or" || arg == "-a" || arg == "-and" || arg == ",";
		}

		static int parsePrimary(Parser& p, const std::string& name) {
			Node node;
			const PrimaryParser parse = findPrimary(name);
			if (parse(p, name, node)) return addNode(p, node);
			if (!p.ok) return -1;

			if (!name.empty() && name[0] == '-') parserFail(p, "unknown predicate `" + name + "'");
			else parserFail(p, "paths must precede expression: `" + name + "'");
			return -1;
		}

		static int parseComma(Parser& p);

		static int parseGroup(Parser& p) {
			if (!atEnd(p) && peek(p) == ")") {
				parserFail(p, "invalid expression; empty parentheses are not allowed.");
				return -1;
			}

			const int inner = parseComma(p);
			if (!p.ok) return -1;

			if (atEnd(p) || peek(p) != ")") {
				parserFail(p, "invalid expression; I was expecting to find a ')' somewhere but did "
					"not see one.");
				return -1;
			}

			++p.pos;
			return inner;
		}

		static int parseUnary(Parser& p) {
			if (atEnd(p)) {
				parserFail(p, "invalid expression; expected an expression");
				return -1;
			}

			const std::string arg = peek(p);
			++p.pos;
			if (arg == "!" || arg == "-not") {
				const int operand = parseUnary(p);
				return p.ok ? combine(p, Op::Not, operand, -1) : -1;
			}

			if (arg == "(") return parseGroup(p);
			if (arg == ")" || isBinaryOperator(arg)) {
				parserFail(p, "invalid expression; you have used a binary operator '" + arg
					+ "' with nothing before it.");
				return -1;
			}

			return parsePrimary(p, arg);
		}

		static bool endsAndChain(const std::string& arg) {
			return arg == ")" || arg == "-o" || arg == "-or" || arg == ",";
		}

		static bool skipOperator(Parser& p) {
			++p.pos;
			if (!atEnd(p)) return true;
			return parserFail(p, "expected an expression after '" + (*p.args)[p.pos - 1] + "'");
		}

		static int parseAnd(Parser& p) {
			int left = parseUnary(p);
			while (p.ok && !atEnd(p) && !endsAndChain(peek(p))) {
				const bool explicit_and = peek(p) == "-a" || peek(p) == "-and";
				if (explicit_and && !skipOperator(p)) return -1;

				const int right = parseUnary(p);
				if (!p.ok) return -1;
				left = combine(p, Op::And, left, right);
			}

			return left;
		}

		static int parseOr(Parser& p) {
			int left = parseAnd(p);
			while (p.ok && !atEnd(p) && (peek(p) == "-o" || peek(p) == "-or")) {
				if (!skipOperator(p)) return -1;

				const int right = parseAnd(p);
				if (!p.ok) return -1;
				left = combine(p, Op::Or, left, right);
			}

			return left;
		}

		static int parseComma(Parser& p) {
			int left = parseOr(p);
			while (p.ok && !atEnd(p) && peek(p) == ",") {
				if (!skipOperator(p)) return -1;

				const int right = parseOr(p);
				if (!p.ok) return -1;
				left = combine(p, Op::Comma, left, right);
			}

			return left;
		}

		static void parseLeadingFlags(Parser& p) {
			Follow& follow = p.run->options.follow;
			while (!atEnd(p)) {
				const std::string& arg = peek(p);
				if (arg == "-P")      follow = Follow::Never;
				else if (arg == "-H") follow = Follow::CommandLine;
				else if (arg == "-L") follow = Follow::Always;
				else if (arg == "--") {
					++p.pos;
					return;
				} else if (arg == "-D" && p.pos + 1 < p.args->size()) {
					++p.pos;
				} else if (arg.compare(0, 2, "-O") != 0 || arg.size() == 2) {
					return;
				}

				++p.pos;
			}
		}

		static bool isExpressionStart(const std::string& arg) {
			if (arg == "(" || arg == ")" || arg == "!" || arg == ",") return true;
			return arg.size() > 1 && arg[0] == '-';
		}

		static void collectRoots(Parser& p) {
			while (!atEnd(p) && !isExpressionStart(peek(p))) {
				p.run->options.roots.push_back(peek(p));
				++p.pos;
			}
		}

		static bool checkDeleteAndPrune(Parser& p) {
			if (!p.has_delete) return true;
			if (p.has_prune && !p.explicit_depth) {
				return parserFail(p, "The -delete action automatically turns on -depth, but -prune "
					"does nothing when -depth is in effect.  If you want to carry on anyway, just "
					"explicitly use the -depth option.");
			}

			p.run->options.depth_first = true;
			return true;
		}

		static int withDefaultPrint(Parser& p, int expression) {
			if (p.has_action) return expression;

			Node print;
			print.test = testPrint;
			const int index = addNode(p, print);
			return expression < 0 ? index : combine(p, Op::And, expression, index);
		}

		static bool parseCommandLine(const std::vector<std::string>& args, Run& run) {
			Parser p;
			p.args = &args;
			p.run = &run;

			parseLeadingFlags(p);
			collectRoots(p);
			const int expression = atEnd(p) ? -1 : parseComma(p);
			if (p.ok && !atEnd(p)) parserFail(p, "you have too many ')'");
			if (!p.ok || !checkDeleteAndPrune(p)) return false;

			run.expression = withDefaultPrint(p, expression);
			if (run.options.roots.empty()) run.options.roots.push_back(".");
			return true;
		}

		static bool evaluate(Run& run, int index, Entry& entry) {
			if (run.quit) return false;

			Node& node = run.nodes[static_cast<std::size_t>(index)];
			switch (node.op) {
				case Op::And:
					return evaluate(run, node.left, entry) && evaluate(run, node.right, entry);
				case Op::Or:
					return evaluate(run, node.left, entry) || evaluate(run, node.right, entry);
				case Op::Not:
					return !evaluate(run, node.left, entry);
				case Op::Comma:
					evaluate(run, node.left, entry);
					return evaluate(run, node.right, entry);
				case Op::Test:
					return node.test(run, node, entry);
				default:
					return false;
			}
		}

		static bool isFollowed(const Run& run, int depth) {
			if (run.options.follow == Follow::Always) return true;
			return run.options.follow == Follow::CommandLine && depth == 0;
		}

		static std::string childPath(const std::string& parent, const std::string& name) {
			if (!parent.empty() && parent.back() == '/') return parent + name;
			return parent + "/" + name;
		}

		static std::vector<fs::path> listDirectory(Run& run, const Entry& entry) {
			std::vector<fs::path> names;
			std::error_code ec;
			fs::directory_iterator it(entry.native, ec);
			const fs::directory_iterator end;
			for (; !ec && it != end; it.increment(ec)) {
				names.push_back(it->path().filename());
			}

			if (ec) reportFindError(run, entry.path, posixErrorText(ec));
			return names;
		}

		static void visit(Run& run, Entry& entry);

		static void visitChildren(Run& run, const Entry& parent) {
			for (const fs::path& name : listDirectory(run, parent)) {
				if (run.quit) return;

				Entry child;
				child.name = pathToUtf8(name);
				child.path = childPath(parent.path, child.name);
				child.root = parent.root;
				child.native = parent.native / name;
				child.depth = parent.depth + 1;
				child.follow = isFollowed(run, child.depth);
				visit(run, child);
			}
		}

		static bool isLoop(Run& run, Entry& entry) {
			const FileStat& stat = entryStat(entry);
			if (!stat.ok) return false;

			for (const Ancestor& ancestor : run.ancestors) {
				if (!(ancestor.identity == stat.identity)) continue;

				std::fflush(stdout);
				std::fprintf(stderr, "find: File system loop detected; '%s' is part of the same "
					"file system loop as '%s'.\n", entry.path.c_str(), ancestor.path.c_str());
				run.status = 1;
				return true;
			}

			return false;
		}

		static void descend(Run& run, Entry& entry) {
			const bool tracked = run.options.follow != Follow::Never;
			if (tracked) run.ancestors.push_back({ entryStat(entry).identity, entry.path });
			visitChildren(run, entry);
			if (tracked) run.ancestors.pop_back();
		}

		static bool shouldDescend(Run& run, Entry& entry) {
			const int max_depth = run.options.max_depth;
			if (entry.pruned || run.quit) return false;
			if (max_depth >= 0 && entry.depth >= max_depth) return false;
			if (entryType(entry) != 'd') return false;
			if (!run.options.same_device || entry.depth == 0) return true;
			return entryStat(entry).identity.volume == run.root_volume;
		}

		static bool isLoopDirectory(Run& run, Entry& entry) {
			if (run.options.follow == Follow::Never || entryType(entry) != 'd') return false;
			return isLoop(run, entry);
		}

		static void visit(Run& run, Entry& entry) {
			if (isLoopDirectory(run, entry)) return;

			const bool applies = entry.depth >= run.options.min_depth;
			if (applies && !run.options.depth_first) evaluate(run, run.expression, entry);
			if (shouldDescend(run, entry)) descend(run, entry);
			if (applies && run.options.depth_first) evaluate(run, run.expression, entry);
		}

		static std::string shownRoot(Executor& exec, const std::string& root) {
			const bool windows_style = root.find('\\') != std::string::npos
				|| (root.size() >= 2 && root[1] == ':');
			return windows_style ? exec.pathConv().toPosix(root) : root;
		}

		static void visitRoot(Run& run, const std::string& root) {
			Entry entry;
			entry.path = shownRoot(*run.exec, root);
			entry.root = entry.path;
			entry.name = baseName(entry.path);
			entry.native = toNative(*run.exec, root);
			entry.follow = isFollowed(run, 0);

			std::error_code ec;
			const fs::file_status status = fs::symlink_status(entry.native, ec);
			if (root.empty() || ec || status.type() == fs::file_type::not_found) {
				reportFindError(run, root, "No such file or directory");
				return;
			}

			if (run.options.same_device) run.root_volume = entryStat(entry).identity.volume;
			run.ancestors.clear();
			visit(run, entry);
		}

		static void closeOutputs(Run& run) {
			for (std::FILE* out : run.outputs) std::fclose(out);
			run.outputs.clear();
		}

		static int builtin_find(Executor& exec, const std::vector<std::string>& args) {
			Run run;
			run.exec = &exec;
			if (!parseCommandLine(args, run)) {
				closeOutputs(run);
				return 1;
			}

			run.now = run.options.day_start ? startOfTomorrowTicks() : currentTicks();
			for (const std::string& root : run.options.roots) {
				if (run.quit) break;
				visitRoot(run, root);
			}

			flushAllBatches(run);
			closeOutputs(run);
			std::fflush(stdout);
			return run.status;
		}

	}  // namespace find_detail

	void registerFindBuiltin(Executor& exec) {
		exec.registerBuiltin("find", find_detail::builtin_find);
	}

}  // namespace wbsh
