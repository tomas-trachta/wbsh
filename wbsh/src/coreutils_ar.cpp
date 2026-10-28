/**
 * @file coreutils_ar.cpp
 * @brief ar: create, update, list and extract `!<arch>` archives.
 *
 * Archives are written in the common GNU layout: a `//` table holds the
 * names longer than fifteen bytes, and headers are deterministic (zero
 * dates and ids, mode 644) unless `U` asks for the real dates. GNU and
 * BSD long names are both understood on read. A symbol index is never
 * written; an existing one is dropped on rewrite, since its member
 * offsets would no longer be true. Every change goes to a sibling
 * temporary file that then replaces the archive.
 */

#include "coreutils_internal.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace ar_detail {

		enum class ArOperation {
			None,
			Delete,
			Move,
			Print,
			QuickAppend,
			Replace,
			Index,
			List,
			Extract,
		};

		enum class ArPosition {
			End,
			After,
			Before,
		};

		enum class ArLoad {
			Loaded,
			Missing,
			Failed,
		};

		struct ArOptions {
			ArOperation operation = ArOperation::None;
			ArPosition position = ArPosition::End;
			bool verbose = false;
			bool quiet_create = false;
			bool only_newer = false;
			bool real_dates = false;
			bool keep_dates = false;
			bool wants_index = false;
			std::string relative_to;
			std::string archive;
			std::vector<std::string> names;
		};

		struct ArMember {
			std::string name;
			std::vector<std::uint8_t> data;
			std::int64_t mtime = 0;
			unsigned long long uid = 0;
			unsigned long long gid = 0;
			unsigned long long mode = 0644;
		};

		struct ArNamePlan {
			std::vector<std::string> fields;
			std::string table;
		};

	}  // namespace ar_detail

	using ar_detail::ArLoad;
	using ar_detail::ArMember;
	using ar_detail::ArNamePlan;
	using ar_detail::ArOperation;
	using ar_detail::ArOptions;
	using ar_detail::ArPosition;

	static const char kArMagic[] = "!<arch>\n";
	static const std::size_t kArMagicSize = 8;
	static const std::size_t kShortNameLimit = 15;
	static const unsigned long long kDeterministicMode = 0644;
	static const unsigned long long kRegularFileMode = 0100644;
	static const std::int64_t kFileTimeEpochOffset = 11644473600LL;
	static const std::size_t kNoMember = static_cast<std::size_t>(-1);

	// Member header: name[16] date[12] uid[6] gid[6] mode[8] (octal) size[10] "`\n",
	// every field left-aligned and padded with spaces.
	static const std::size_t kHeaderSize  = 60;
	static const std::size_t kNameWidth   = 16;
	static const std::size_t kDateOffset  = 16;
	static const std::size_t kDateWidth   = 12;
	static const std::size_t kUidOffset   = 28;
	static const std::size_t kGidOffset   = 34;
	static const std::size_t kIdWidth     = 6;
	static const std::size_t kModeOffset  = 40;
	static const std::size_t kModeWidth   = 8;
	static const std::size_t kSizeOffset  = 48;
	static const std::size_t kSizeWidth   = 10;

	static ArOperation operationForLetter(char letter) {
		switch (letter) {
		case 'd': return ArOperation::Delete;
		case 'm': return ArOperation::Move;
		case 'p': return ArOperation::Print;
		case 'q': return ArOperation::QuickAppend;
		case 'r': return ArOperation::Replace;
		case 't': return ArOperation::List;
		case 'x': return ArOperation::Extract;
		default:  return ArOperation::None;
		}
	}

	static bool applyArModifier(char letter, ArOptions& options) {
		switch (letter) {
		case 'v': options.verbose = true;                break;
		case 'c': options.quiet_create = true;           break;
		case 'u': options.only_newer = true;             break;
		case 'D': options.real_dates = false;            break;
		case 'U': options.real_dates = true;             break;
		case 'o': options.keep_dates = true;             break;
		case 's': options.wants_index = true;            break;
		case 'a': options.position = ArPosition::After;  break;
		case 'b':
		case 'i': options.position = ArPosition::Before; break;
		case 'S':
		case 'l': break;
		default:  return false;
		}

		return true;
	}

	static bool applyArKey(const std::string& key, ArOptions& options) {
		for (const char letter : key) {
			const ArOperation operation = operationForLetter(letter);
			if (operation == ArOperation::None) {
				if (applyArModifier(letter, options)) continue;
				perr("ar", std::string("invalid option -- '") + letter + "'");
				return false;
			}

			if (options.operation != ArOperation::None && options.operation != operation) {
				perr("ar", "two different operation options specified");
				return false;
			}

			options.operation = operation;
		}

		return true;
	}

	static bool isOptionCluster(const std::string& arg) {
		return arg.size() > 1 && arg[0] == '-';
	}

	static bool parseArKeys(const std::vector<std::string>& args, std::size_t& at,
	                        ArOptions& options) {
		for (; at < args.size() && isOptionCluster(args[at]); ++at) {
			if (!applyArKey(args[at].substr(1), options)) return false;
		}

		const bool needs_key = options.operation == ArOperation::None && !options.wants_index;
		if (needs_key && at < args.size() && !applyArKey(args[at++], options)) return false;
		if (options.operation == ArOperation::None && options.wants_index) {
			options.operation = ArOperation::Index;
		}

		return options.operation != ArOperation::None;
	}

	static bool parseArArgs(const std::vector<std::string>& args, ArOptions& options) {
		std::size_t at = 0;
		if (!parseArKeys(args, at, options)) return false;

		if (options.position != ArPosition::End) {
			if (at >= args.size()) return false;
			options.relative_to = args[at++];
		}

		if (at >= args.size()) return false;
		options.archive = args[at++];
		options.names.assign(args.begin() + static_cast<std::ptrdiff_t>(at), args.end());
		return true;
	}

	static std::string memberNameOf(const std::string& path) {
		const std::size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	static void readStream(std::FILE* in, std::vector<std::uint8_t>& bytes) {
		std::uint8_t chunk[65536];
		for (std::size_t got = 0; (got = std::fread(chunk, 1, sizeof(chunk), in)) != 0;) {
			bytes.insert(bytes.end(), chunk, chunk + got);
		}
	}

	static std::string fieldText(const std::uint8_t* header, std::size_t offset,
	                             std::size_t width) {
		std::string text(reinterpret_cast<const char*>(header + offset), width);
		while (!text.empty() && text.back() == ' ') text.pop_back();
		return text;
	}

	static unsigned long long fieldNumber(const std::uint8_t* header, std::size_t offset,
	                                      std::size_t width, int base) {
		unsigned long long value = 0;
		parseULL(fieldText(header, offset, width), value, nullptr, base);
		return value;
	}

	static std::string longArName(const std::string& table, std::size_t offset) {
		if (offset >= table.size()) return std::string();
		const std::size_t end = table.find_first_of("/\n", offset);
		return table.substr(offset, end == std::string::npos ? std::string::npos : end - offset);
	}

	static std::string takeBsdName(const std::uint8_t*& data, std::size_t& size,
	                               std::size_t length) {
		const std::string stored(reinterpret_cast<const char*>(data), length);
		data += length;
		size -= length;
		return stored.substr(0, ::strnlen(stored.c_str(), stored.size()));
	}

	// GNU stores long names as "/OFFSET" into the "//" member; BSD as
	// "#1/LENGTH" with the name at the start of the member's data.
	static std::string resolveArName(std::string name, const std::string& long_names,
	                                 const std::uint8_t*& data, std::size_t& size) {
		unsigned long long reference = 0;
		if (name.size() > 1 && name[0] == '/' && parseULL(name.substr(1), reference)) {
			return longArName(long_names, static_cast<std::size_t>(reference));
		}

		const bool bsd = name.compare(0, 3, "#1/") == 0 && parseULL(name.substr(3), reference);
		const std::size_t length = static_cast<std::size_t>(reference);
		if (bsd && reference <= size) return takeBsdName(data, size, length);

		if (!name.empty() && name.back() == '/') name.pop_back();
		return name;
	}

	static bool isSymbolIndex(const std::string& raw_name, const std::string& name) {
		return raw_name == "/" || raw_name == "/SYM64/" || name.compare(0, 9, "__.SYMDEF") == 0;
	}

	static void takeMember(std::string& long_names, const std::uint8_t* header,
	                       const std::uint8_t* data, std::size_t size, std::vector<ArMember>& members) {
		const std::string raw_name = fieldText(header, 0, kNameWidth);
		if (raw_name == "//") {
			long_names.assign(reinterpret_cast<const char*>(data), size);
			return;
		}

		ArMember member;
		member.name = resolveArName(raw_name, long_names, data, size);
		if (isSymbolIndex(raw_name, member.name)) return;

		member.mtime = static_cast<std::int64_t>(fieldNumber(header, kDateOffset, kDateWidth, 10));
		member.uid = fieldNumber(header, kUidOffset, kIdWidth, 10);
		member.gid = fieldNumber(header, kGidOffset, kIdWidth, 10);
		member.mode = fieldNumber(header, kModeOffset, kModeWidth, 8);
		member.data.assign(data, data + size);
		members.push_back(member);
	}

	static bool parseMembers(const std::vector<std::uint8_t>& bytes,
	                         std::vector<ArMember>& members) {
		std::string long_names;
		std::size_t at = kArMagicSize;
		while (at + kHeaderSize <= bytes.size()) {
			const std::uint8_t* header = bytes.data() + at;
			const unsigned long long size = fieldNumber(header, kSizeOffset, kSizeWidth, 10);
			const std::size_t data_at = at + kHeaderSize;
			if (size > bytes.size() - data_at) return false;

			const std::size_t length = static_cast<std::size_t>(size);
			at = data_at + length + length % 2;
			takeMember(long_names, header, bytes.data() + data_at, length, members);
		}

		return true;
	}

	static ArLoad loadArchive(Executor& exec, const std::string& path,
	                          std::vector<ArMember>& members) {
		std::FILE* in = fopenNative(exec, path, "rb");
		if (in == nullptr && errno == ENOENT) return ArLoad::Missing;
		if (in == nullptr) {
			perr("ar", path + ": " + std::strerror(errno));
			return ArLoad::Failed;
		}

		std::vector<std::uint8_t> bytes;
		readStream(in, bytes);
		std::fclose(in);
		const bool tagged = bytes.size() >= kArMagicSize
			&& std::memcmp(bytes.data(), kArMagic, kArMagicSize) == 0;
		if (tagged && parseMembers(bytes, members)) return ArLoad::Loaded;

		perr("ar", path + ": not an archive");
		return ArLoad::Failed;
	}

	static std::string paddedField(const std::string& text, std::size_t width) {
		std::string field = text.substr(0, width);
		field.append(width - field.size(), ' ');
		return field;
	}

	static std::string octalText(unsigned long long value) {
		char text[32];
		std::snprintf(text, sizeof(text), "%llo", value);
		return text;
	}

	static ArNamePlan planNames(const std::vector<ArMember>& members) {
		ArNamePlan plan;
		for (const ArMember& member : members) {
			if (member.name.size() <= kShortNameLimit) {
				plan.fields.push_back(member.name + "/");
				continue;
			}

			plan.fields.push_back("/" + std::to_string(plan.table.size()));
			plan.table += member.name + "/\n";
		}

		return plan;
	}

	static std::string memberHeader(const std::string& name_field, const ArMember& member) {
		return paddedField(name_field, kNameWidth)
			+ paddedField(std::to_string(member.mtime), kDateWidth)
			+ paddedField(std::to_string(member.uid), kIdWidth)
			+ paddedField(std::to_string(member.gid), kIdWidth)
			+ paddedField(octalText(member.mode), kModeWidth)
			+ paddedField(std::to_string(member.data.size()), kSizeWidth)
			+ "`\n";
	}

	static std::string nameTableHeader(std::size_t size) {
		return paddedField("//", kNameWidth)
			+ paddedField(std::string(), kDateWidth + 2 * kIdWidth + kModeWidth)
			+ paddedField(std::to_string(size), kSizeWidth)
			+ "`\n";
	}

	static void writeBytes(std::FILE* out, const void* data, std::size_t size, bool& ok) {
		if (ok && size != 0) ok = std::fwrite(data, 1, size, out) == size;
	}

	static void writeEvenPadding(std::FILE* out, std::size_t size, bool& ok) {
		if (size % 2 != 0) writeBytes(out, "\n", 1, ok);
	}

	static bool writeMembers(std::FILE* out, const std::vector<ArMember>& members) {
		const ArNamePlan plan = planNames(members);
		bool ok = true;
		writeBytes(out, kArMagic, kArMagicSize, ok);
		if (!plan.table.empty()) {
			const std::string header = nameTableHeader(plan.table.size());
			writeBytes(out, header.data(), header.size(), ok);
			writeBytes(out, plan.table.data(), plan.table.size(), ok);
			writeEvenPadding(out, plan.table.size(), ok);
		}

		for (std::size_t k = 0; k < members.size(); ++k) {
			const std::string header = memberHeader(plan.fields[k], members[k]);
			writeBytes(out, header.data(), header.size(), ok);
			writeBytes(out, members[k].data.data(), members[k].data.size(), ok);
			writeEvenPadding(out, members[k].data.size(), ok);
		}

		return ok;
	}

	static bool replaceWithTemp(const fs::path& temp, const fs::path& target) {
		return ::MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
	}

	static bool saveArchive(Executor& exec, const std::string& path,
	                        const std::vector<ArMember>& members) {
		const fs::path target = toNative(exec, path);
		fs::path temp;
		std::FILE* out = createSiblingTempFile(target, "ar", temp);
		if (out == nullptr) {
			perr("ar", path + ": " + std::strerror(errno));
			return false;
		}

		bool ok = writeMembers(out, members);
		ok = std::fclose(out) == 0 && ok;
		if (ok && replaceWithTemp(temp, target)) return true;

		perr("ar", path + ": cannot write the archive");
		std::error_code ec;
		fs::remove(temp, ec);
		return false;
	}

	static std::int64_t mtimeOf(const fs::path& path) {
		std::error_code ec;
		const auto stamp = fs::last_write_time(path, ec);
		if (ec) return 0;
		return std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count()
			- kFileTimeEpochOffset;
	}

	static bool loadInputFile(Executor& exec, const std::string& path, ArMember& member) {
		const fs::path native = toNative(exec, path);
		std::error_code ec;
		if (fs::is_directory(native, ec)) {
			perr("ar", path + ": " + std::strerror(EISDIR));
			return false;
		}

		std::FILE* in = fopenNative(exec, path, "rb");
		if (in == nullptr) {
			perr("ar", path + ": " + std::strerror(errno));
			return false;
		}

		readStream(in, member.data);
		std::fclose(in);
		member.name = memberNameOf(path);
		member.mtime = mtimeOf(native);
		member.mode = kRegularFileMode;
		return true;
	}

	static bool loadInputFiles(Executor& exec, const ArOptions& options,
	                           std::vector<ArMember>& incoming) {
		for (const std::string& path : options.names) {
			ArMember member;
			if (!loadInputFile(exec, path, member)) return false;
			incoming.push_back(member);
		}

		return true;
	}

	static ArMember storedForm(const ArOptions& options, const ArMember& member) {
		ArMember stored = member;
		if (options.real_dates) return stored;

		stored.mtime = 0;
		stored.mode = kDeterministicMode;
		return stored;
	}

	static std::size_t findMember(const std::vector<ArMember>& members, const std::string& name) {
		const std::string wanted = memberNameOf(name);
		for (std::size_t k = 0; k < members.size(); ++k) {
			if (members[k].name == wanted) return k;
		}

		return kNoMember;
	}

	static std::size_t insertionIndex(const ArOptions& options,
	                                  const std::vector<ArMember>& members, std::size_t fallback) {
		if (options.position == ArPosition::End) return fallback;

		const std::size_t anchor = findMember(members, options.relative_to);
		if (anchor == kNoMember) return members.size();
		return options.position == ArPosition::After ? anchor + 1 : anchor;
	}

	static void insertAt(std::vector<ArMember>& members, std::size_t index,
	                     const ArMember& member) {
		members.insert(members.begin() + static_cast<std::ptrdiff_t>(index), member);
	}

	static void report(const ArOptions& options, char action, const std::string& name) {
		if (options.verbose) std::printf("%c - %s\n", action, name.c_str());
	}

	static void replaceMember(const ArOptions& options, std::vector<ArMember>& members,
	                          const ArMember& incoming) {
		const ArMember stored = storedForm(options, incoming);
		const std::size_t existing = findMember(members, incoming.name);
		if (existing == kNoMember) {
			report(options, 'a', incoming.name);
			insertAt(members, insertionIndex(options, members, members.size()), stored);
			return;
		}

		if (options.only_newer && members[existing].mtime >= incoming.mtime) return;

		report(options, 'r', incoming.name);
		members.erase(members.begin() + static_cast<std::ptrdiff_t>(existing));
		insertAt(members, insertionIndex(options, members, existing), stored);
	}

	static bool replaceMembers(Executor& exec, const ArOptions& options,
	                           std::vector<ArMember>& members) {
		std::vector<ArMember> incoming;
		if (!loadInputFiles(exec, options, incoming)) return false;

		for (const ArMember& member : incoming) replaceMember(options, members, member);
		return true;
	}

	static bool appendMembers(Executor& exec, const ArOptions& options,
	                          std::vector<ArMember>& members) {
		std::vector<ArMember> incoming;
		if (!loadInputFiles(exec, options, incoming)) return false;

		for (const ArMember& member : incoming) {
			report(options, 'a', member.name);
			members.push_back(storedForm(options, member));
		}

		return true;
	}

	static bool eraseMember(const ArOptions& options, std::vector<ArMember>& members,
	                        const std::string& name) {
		const std::size_t found = findMember(members, name);
		if (found == kNoMember) return false;

		report(options, 'd', members[found].name);
		members.erase(members.begin() + static_cast<std::ptrdiff_t>(found));
		return true;
	}

	static void deleteMembers(const ArOptions& options, std::vector<ArMember>& members) {
		for (const std::string& name : options.names) {
			bool found = false;
			while (eraseMember(options, members, name)) found = true;

			if (!found && options.verbose) std::printf("No member named `%s'\n", name.c_str());
		}
	}

	static bool moveMembers(const ArOptions& options, std::vector<ArMember>& members) {
		for (const std::string& name : options.names) {
			const std::size_t found = findMember(members, name);
			if (found == kNoMember) {
				perr("ar", "no entry " + name + " in archive " + options.archive);
				return false;
			}

			const ArMember member = members[found];
			report(options, 'm', member.name);
			members.erase(members.begin() + static_cast<std::ptrdiff_t>(found));
			insertAt(members, insertionIndex(options, members, members.size()), member);
		}

		return true;
	}

	static bool createsArchive(ArOperation operation) {
		return operation == ArOperation::Replace || operation == ArOperation::QuickAppend;
	}

	static bool applyChange(Executor& exec, const ArOptions& options,
	                        std::vector<ArMember>& members) {
		switch (options.operation) {
		case ArOperation::Replace:     return replaceMembers(exec, options, members);
		case ArOperation::QuickAppend: return appendMembers(exec, options, members);
		case ArOperation::Move:        return moveMembers(options, members);
		case ArOperation::Delete:
			deleteMembers(options, members);
			return true;
		default:
			return true;
		}
	}

	static int modifyArchive(Executor& exec, const ArOptions& options) {
		std::vector<ArMember> members;
		const ArLoad load = loadArchive(exec, options.archive, members);
		if (load == ArLoad::Failed) return 1;
		if (load == ArLoad::Missing && !createsArchive(options.operation)) {
			perr("ar", options.archive + ": " + std::strerror(ENOENT));
			return 1;
		}

		const bool creating = load == ArLoad::Missing;
		if (creating && !options.quiet_create) perr("ar", "creating " + options.archive);
		if (!applyChange(exec, options, members)) return 1;
		if (options.operation == ArOperation::Index) return 0;
		return saveArchive(exec, options.archive, members) ? 0 : 1;
	}

	static std::string permissionText(unsigned long long mode) {
		static const char kLetters[] = "rwxrwxrwx";
		std::string text(9, '-');
		for (std::size_t bit = 0; bit < 9; ++bit) {
			if ((mode & (0400ULL >> bit)) != 0) text[bit] = kLetters[bit];
		}

		return text;
	}

	static std::string listingTime(std::int64_t mtime) {
		const std::time_t when = static_cast<std::time_t>(mtime);
		std::tm local{};
		if (localtime_s(&local, &when) != 0) return std::string();

		char text[32];
		std::strftime(text, sizeof(text), "%b %e %H:%M %Y", &local);
		return text;
	}

	static void listMember(const ArOptions& options, const ArMember& member) {
		if (!options.verbose) {
			std::printf("%s\n", member.name.c_str());
			return;
		}

		std::printf("%s %llu/%llu %6zu %s %s\n", permissionText(member.mode).c_str(), member.uid,
			member.gid, member.data.size(), listingTime(member.mtime).c_str(), member.name.c_str());
	}

	static void printMember(const ArOptions& options, const ArMember& member) {
		if (options.verbose) std::printf("\n<%s>\n\n", member.name.c_str());
		std::fwrite(member.data.data(), 1, member.data.size(), stdout);
	}

	static void restoreDate(const fs::path& path, std::int64_t mtime) {
		const auto since = std::chrono::seconds(mtime + kFileTimeEpochOffset);
		std::error_code ec;
		fs::last_write_time(path, fs::file_time_type(
			std::chrono::duration_cast<fs::file_time_type::duration>(since)), ec);
	}

	static bool isPlainFileName(const std::string& name) {
		return !name.empty() && name != "." && name != ".."
			&& name.find_first_of("/\\:") == std::string::npos;
	}

	static bool extractMember(Executor& exec, const ArOptions& options, const ArMember& member) {
		if (!isPlainFileName(member.name)) {
			perr("ar", member.name + ": illegal output pathname for archive member");
			return false;
		}

		report(options, 'x', member.name);
		std::FILE* out = fopenNative(exec, member.name, "wb");
		if (out == nullptr) {
			perr("ar", member.name + ": " + std::strerror(errno));
			return false;
		}

		const std::size_t size = member.data.size();
		const bool written = std::fwrite(member.data.data(), 1, size, out) == size;
		const bool closed = std::fclose(out) == 0;
		if (options.keep_dates) restoreDate(toNative(exec, member.name), member.mtime);
		return written && closed;
	}

	static bool visitMember(Executor& exec, const ArOptions& options, const ArMember& member) {
		switch (options.operation) {
		case ArOperation::List:    listMember(options, member);                 return true;
		case ArOperation::Print:   printMember(options, member);                return true;
		case ArOperation::Extract: return extractMember(exec, options, member);
		default:                   return true;
		}
	}

	static bool memberWanted(const ArOptions& options, const ArMember& member,
	                         std::vector<bool>& seen) {
		if (options.names.empty()) return true;

		bool wanted = false;
		for (std::size_t k = 0; k < options.names.size(); ++k) {
			if (memberNameOf(options.names[k]) != member.name) continue;
			seen[k] = true;
			wanted = true;
		}

		return wanted;
	}

	static bool reportUnseen(const ArOptions& options, const std::vector<bool>& seen) {
		bool all_seen = true;
		for (std::size_t k = 0; k < seen.size(); ++k) {
			if (seen[k]) continue;
			perr("ar", "no entry " + options.names[k] + " in archive");
			all_seen = false;
		}

		return all_seen;
	}

	static int readArchive(Executor& exec, const ArOptions& options) {
		std::vector<ArMember> members;
		const ArLoad load = loadArchive(exec, options.archive, members);
		if (load == ArLoad::Missing) perr("ar", options.archive + ": " + std::strerror(ENOENT));
		if (load != ArLoad::Loaded) return 1;

		int status = 0;
		std::vector<bool> seen(options.names.size(), false);
		for (const ArMember& member : members) {
			if (!memberWanted(options, member, seen)) continue;
			if (!visitMember(exec, options, member)) status = 1;
		}

		if (!reportUnseen(options, seen)) status = 1;
		std::fflush(stdout);
		return status;
	}

	static bool isReadOperation(ArOperation operation) {
		return operation == ArOperation::List || operation == ArOperation::Print
			|| operation == ArOperation::Extract;
	}

	static int builtin_ar(Executor& exec, const std::vector<std::string>& args) {
		ArOptions options;
		if (!parseArArgs(args, options)) {
			perr("ar", "usage: ar [-]{d|m|p|q|r|s|t|x}[abcDiosuUv] [MEMBER] ARCHIVE [FILE]...");
			return 1;
		}

		if (isReadOperation(options.operation)) return readArchive(exec, options);
		return modifyArchive(exec, options);
	}

	void registerArBuiltin(Executor& exec) {
		exec.registerBuiltin("ar", builtin_ar);
	}

}  // namespace wbsh
