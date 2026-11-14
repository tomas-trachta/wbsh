/**
 * @file filemode.cpp
 * @brief Permission bits of Windows files: defaults, the `wbsh.mode`
 *        stream chmod leaves behind, and chmod's mode syntax.
 *
 * The stream holds the mode as octal text. NTFS keeps it through renames and
 * CopyFile; filesystems without streams (FAT, exFAT, some shares) keep only
 * the write bit, as a noexec mount does on Linux.
 */

#include "filemode.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif /* _WIN32 */

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	namespace filemode_detail {

		struct ModeClause {
			unsigned who = 0;
			char op = 0;
			unsigned bits = 0;
		};

	}  // namespace filemode_detail

	using filemode_detail::ModeClause;

	static const unsigned kPermissionBits = 0777;
	static const unsigned kModeBits = 07777;
	static const unsigned kOwnerWrite = 0200;
	static const unsigned kOwnerExecute = 0100;
	static const unsigned kAnyWrite = 0222;
	static const unsigned kAnyExecute = 0111;
	static const unsigned kTypeMask = 0170000;
	static const unsigned kTypeDirectory = 0040000;
	static const unsigned kProgramMode = 0755;
	static const unsigned kDataMode = 0644;
	static const std::size_t kStoredModeLength = 8;

	static const char* const kProgramExtensions[] = {
		".exe", ".com", ".bat", ".cmd", ".ps1", ".sh",
	};

	static bool hasProgramExtension(const fs::path& native) {
		std::string extension = pathToUtf8(native.extension());
		for (char& c : extension) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}

		for (const char* program : kProgramExtensions) {
			if (extension == program) return true;
		}

		return false;
	}

	static bool isProgramMagic(const char* magic) {
		return (magic[0] == '#' && magic[1] == '!') || (magic[0] == 'M' && magic[1] == 'Z');
	}

	static bool parseStoredMode(const std::string& text, unsigned& mode) {
		unsigned long value = 0;
		if (!parseUL(text, value, 8) || value > kPermissionBits) return false;

		mode = static_cast<unsigned>(value);
		return true;
	}

#ifdef _WIN32
	static const DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

	// Absolute, so `.` and `..` name a directory a stream can hang on.
	static std::wstring modeStreamPath(const fs::path& native) {
		std::error_code ec;
		const fs::path full = fs::absolute(native, ec);
		std::wstring path = ec ? native.wstring() : full.wstring();
		while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
		return path + L":wbsh.mode";
	}

	static bool startsWithProgramMagic(const fs::path& native) {
		const HANDLE file = ::CreateFileW(native.c_str(), GENERIC_READ, kShareAll, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;

		char magic[2] = { 0, 0 };
		DWORD got = 0;
		const BOOL read = ::ReadFile(file, magic, sizeof(magic), &got, nullptr);
		::CloseHandle(file);
		return read != FALSE && got == sizeof(magic) && isProgramMagic(magic);
	}

	static bool readStoredMode(const fs::path& native, unsigned& mode) {
		const HANDLE stream = ::CreateFileW(modeStreamPath(native).c_str(), GENERIC_READ, kShareAll,
			nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (stream == INVALID_HANDLE_VALUE) return false;

		char text[kStoredModeLength] = {};
		DWORD got = 0;
		const BOOL read = ::ReadFile(stream, text, sizeof(text), &got, nullptr);
		::CloseHandle(stream);
		return read != FALSE && parseStoredMode(std::string(text, got), mode);
	}

	static bool writeStoredMode(const fs::path& native, unsigned mode) {
		const HANDLE stream = ::CreateFileW(modeStreamPath(native).c_str(), GENERIC_WRITE, 0,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (stream == INVALID_HANDLE_VALUE) return false;

		char text[kStoredModeLength];
		const int length = std::snprintf(text, sizeof(text), "%03o", mode & kPermissionBits);
		DWORD written = 0;
		const BOOL ok = ::WriteFile(stream, text, static_cast<DWORD>(length), &written, nullptr);
		::CloseHandle(stream);
		return ok != FALSE;
	}

	static bool removeStoredMode(const fs::path& native) {
		return ::DeleteFileW(modeStreamPath(native).c_str()) != FALSE;
	}

	// Writing or deleting a stream bumps the file's modification time, which
	// chmod must leave alone.
	static void restoreWriteTime(const fs::path& native, const FILETIME& when) {
		const HANDLE file = ::CreateFileW(native.c_str(), FILE_WRITE_ATTRIBUTES, kShareAll, nullptr,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (file == INVALID_HANDLE_VALUE) return;

		::SetFileTime(file, nullptr, nullptr, &when);
		::CloseHandle(file);
	}
#else
	static bool startsWithProgramMagic(const fs::path&) {
		return false;
	}

	static bool readStoredMode(const fs::path&, unsigned&) {
		return false;
	}
#endif /* _WIN32 */

	static bool executableByDefault(const fs::path& native) {
		return hasProgramExtension(native) || startsWithProgramMagic(native);
	}

	static unsigned defaultPermissions(const fs::path& native, bool is_directory) {
		if (is_directory) return kProgramMode;
		return executableByDefault(native) ? kProgramMode : kDataMode;
	}

	unsigned filePermissions(const fs::path& native, bool is_directory, bool read_only) {
		unsigned stored = 0;
		const unsigned bits = readStoredMode(native, stored)
			? stored
			: defaultPermissions(native, is_directory);

		return read_only ? (bits & ~kAnyWrite) : (bits | kOwnerWrite);
	}

	bool hasOwnerExecute(const fs::path& native, bool is_directory) {
		unsigned stored = 0;
		if (readStoredMode(native, stored)) return (stored & kOwnerExecute) != 0;

		return is_directory || executableByDefault(native);
	}

#ifdef _WIN32
	// True when the stream was written or removed.
	static bool storeMode(const fs::path& native, unsigned mode, bool is_directory) {
		const unsigned wanted = (mode & kPermissionBits) | kOwnerWrite;
		unsigned stored = 0;
		const bool has_stored = readStoredMode(native, stored);
		if (wanted == defaultPermissions(native, is_directory)) {
			return has_stored && removeStoredMode(native);
		}

		if (has_stored && (stored | kOwnerWrite) == wanted) return false;
		return writeStoredMode(native, wanted);
	}

	static bool setAttributes(const fs::path& native, DWORD attributes, std::error_code& ec) {
		if (::SetFileAttributesW(native.c_str(), attributes) != FALSE) return true;

		ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
		return false;
	}

	// The stream of a read-only file cannot be written, so the attribute is
	// cleared first and set again afterwards when the mode wants it.
	bool setFilePermissions(const fs::path& native, unsigned mode, std::error_code& ec) {
		WIN32_FILE_ATTRIBUTE_DATA data{};
		if (::GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data) == FALSE) {
			ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
			return false;
		}

		const DWORD writable = data.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
		if (writable != data.dwFileAttributes && !setAttributes(native, writable, ec)) return false;

		const bool is_directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (storeMode(native, mode, is_directory)) restoreWriteTime(native, data.ftLastWriteTime);

		if ((mode & kOwnerWrite) != 0) return true;
		return setAttributes(native, writable | FILE_ATTRIBUTE_READONLY, ec);
	}
#else
	bool setFilePermissions(const fs::path& native, unsigned mode, std::error_code& ec) {
		fs::permissions(native, static_cast<fs::perms>(mode & kModeBits), ec);
		return !ec;
	}
#endif /* _WIN32 */

	static unsigned whoBits(char c) {
		switch (c) {
			case 'u':
				return 04700;
			case 'g':
				return 02070;
			case 'o':
				return 01007;
			case 'a':
				return 07777;
			default:
				return 0;
		}
	}

	static unsigned permissionBits(char c, unsigned mode) {
		switch (c) {
			case 'r':
				return 0444;
			case 'w':
				return 0222;
			case 'x':
				return 0111;
			case 'X':
				return (mode & kAnyExecute) != 0 || (mode & kTypeMask) == kTypeDirectory ? 0111 : 0;
			case 's':
				return 06000;
			case 't':
				return 01000;
			default:
				return 0;
		}
	}

	static unsigned copiedBits(char c, unsigned mode) {
		const unsigned shift = c == 'u' ? 6 : c == 'g' ? 3 : 0;
		const unsigned rwx = (mode >> shift) & 07;
		return rwx * 0111;
	}

	// GNU semantics: a clause without u/g/o/a touches only the bits the
	// umask lets through, and `=` without one clears every bit first.
	static unsigned applyClause(const ModeClause& clause, unsigned umask, unsigned mode) {
		const unsigned who = clause.who == 0 ? (kPermissionBits & ~umask) : clause.who;
		const unsigned affected = who & clause.bits;
		if (clause.op == '+') return mode | affected;
		if (clause.op == '-') return mode & ~affected;

		const unsigned cleared = mode & ~(clause.who == 0 ? kModeBits : clause.who);
		return cleared | affected;
	}

	static bool isOperator(char c) {
		return c == '+' || c == '-' || c == '=';
	}

	static bool readClauseBits(const std::string& text, std::size_t& at, unsigned mode,
	                           unsigned& bits) {
		bits = 0;
		const bool copies = at < text.size() && whoBits(text[at]) != 0 && text[at] != 'a';
		if (copies) {
			bits = copiedBits(text[at++], mode);
			return true;
		}

		for (; at < text.size() && text[at] != ',' && !isOperator(text[at]); ++at) {
			const unsigned one = permissionBits(text[at], mode);
			if (one == 0 && text[at] != 'X') return false;
			bits |= one;
		}

		return true;
	}

	static bool applySymbolicPart(const std::string& text, std::size_t& at, unsigned umask,
	                              unsigned& mode) {
		ModeClause clause;
		for (; at < text.size() && whoBits(text[at]) != 0; ++at) clause.who |= whoBits(text[at]);
		if (at >= text.size() || !isOperator(text[at])) return false;

		while (at < text.size() && isOperator(text[at])) {
			clause.op = text[at++];
			if (!readClauseBits(text, at, mode, clause.bits)) return false;
			mode = applyClause(clause, umask, mode);
		}

		return at >= text.size() || text[at] == ',';
	}

	static bool applySymbolicMode(const std::string& text, unsigned umask, unsigned& mode) {
		unsigned result = mode;
		std::size_t at = 0;
		while (at < text.size()) {
			if (!applySymbolicPart(text, at, umask, result)) return false;
			if (at < text.size()) ++at;
		}

		if (text.empty() || text.back() == ',') return false;

		mode = result;
		return true;
	}

	static bool applyOctalMode(const std::string& text, unsigned& mode) {
		unsigned long octal = 0;
		if (!parseUL(text, octal, 8) || octal > kModeBits) return false;

		mode = (mode & ~kModeBits) | static_cast<unsigned>(octal);
		return true;
	}

	bool applyModeText(const std::string& text, unsigned umask, unsigned& mode) {
		const bool octal = !text.empty() && text.find_first_not_of("01234567") == std::string::npos;
		if (octal) return applyOctalMode(text, mode);

		return applySymbolicMode(text, umask, mode);
	}

}  // namespace wbsh
