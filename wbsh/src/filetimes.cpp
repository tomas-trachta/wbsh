/**
 * @file filetimes.cpp
 * @brief A file's four timestamps with full precision, read and set.
 */

#include "filetimes.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <chrono>
#include <filesystem>

namespace wbsh {

#ifdef _WIN32
	// FILETIME counts 100 ns ticks from 1601-01-01; this many lie before 1970.
	static const long long kTicksTo1970 = 116444736000000000LL;
	static const long long kTicksPerSecond = 10000000LL;
	static const long kNanosPerTick = 100;

	static DateValue dateFromTicks(long long ticks) {
		const long long since_1970 = ticks - kTicksTo1970;
		long long seconds = since_1970 / kTicksPerSecond;
		long long rest = since_1970 % kTicksPerSecond;
		if (rest < 0) {
			rest += kTicksPerSecond;
			--seconds;
		}

		return { seconds, static_cast<long>(rest) * kNanosPerTick };
	}

	static FILETIME fileTimeFrom(const DateValue& value) {
		const long long ticks = value.seconds * kTicksPerSecond + value.nanos / kNanosPerTick
			+ kTicksTo1970;
		FILETIME time{};
		time.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFLL);
		time.dwHighDateTime = static_cast<DWORD>(static_cast<unsigned long long>(ticks) >> 32);
		return time;
	}

	static HANDLE openForTimes(const std::filesystem::path& native, DWORD access, bool follow) {
		DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
		if (!follow) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
		return ::CreateFileW(native.c_str(), access,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			flags, nullptr);
	}

	bool readFileTimes(const std::filesystem::path& native, bool follow, FileTimes& times) {
		const HANDLE handle = openForTimes(native, FILE_READ_ATTRIBUTES, follow);
		if (handle == INVALID_HANDLE_VALUE) return false;

		FILE_BASIC_INFO info{};
		const BOOL got = ::GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof(info));
		const bool ok = got != FALSE;
		::CloseHandle(handle);
		if (!ok) return false;

		times.access = dateFromTicks(info.LastAccessTime.QuadPart);
		times.modify = dateFromTicks(info.LastWriteTime.QuadPart);
		times.change = dateFromTicks(info.ChangeTime.QuadPart);
		times.birth = dateFromTicks(info.CreationTime.QuadPart);
		return true;
	}

	bool writeFileTimes(const std::filesystem::path& native, const DateValue* access,
	                    const DateValue* modify) {
		const HANDLE handle = openForTimes(native, FILE_WRITE_ATTRIBUTES, true);
		if (handle == INVALID_HANDLE_VALUE) return false;

		const FILETIME access_time = access != nullptr ? fileTimeFrom(*access) : FILETIME{};
		const FILETIME modify_time = modify != nullptr ? fileTimeFrom(*modify) : FILETIME{};
		const BOOL ok = ::SetFileTime(handle, nullptr,
			access != nullptr ? &access_time : nullptr,
			modify != nullptr ? &modify_time : nullptr);
		::CloseHandle(handle);
		return ok != FALSE;
	}

	// MSVC's file clock counts FILETIME ticks.
	DateValue dateFromFileTime(const std::filesystem::file_time_type& time) {
		return dateFromTicks(static_cast<long long>(time.time_since_epoch().count()));
	}
#else
	DateValue dateFromFileTime(const std::filesystem::file_time_type& time) {
		using namespace std::chrono;
		const auto system_time = time_point_cast<nanoseconds>(
			time - std::filesystem::file_time_type::clock::now() + system_clock::now());
		const long long nanos = system_time.time_since_epoch().count();
		return { nanos / 1000000000LL, static_cast<long>(nanos % 1000000000LL) };
	}

	bool readFileTimes(const std::filesystem::path&, bool, FileTimes&) {
		return false;
	}

	bool writeFileTimes(const std::filesystem::path&, const DateValue*, const DateValue*) {
		return false;
	}
#endif /* _WIN32 */

}  // namespace wbsh
