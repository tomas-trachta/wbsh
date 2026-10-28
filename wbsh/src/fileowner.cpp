/**
 * @file fileowner.cpp
 * @brief Owner and group SIDs of files: name lookup, reading and setting.
 *
 * Files are opened by handle (FILE_FLAG_BACKUP_SEMANTICS, so directories
 * open too, and FILE_FLAG_OPEN_REPARSE_POINT for the link itself) and
 * read with GetSecurityInfo / set with SetSecurityInfo. Names are the bare
 * account names `ps` shows; a SID without an account shows as its string.
 */

#include "fileowner.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <aclapi.h>
#  include <sddl.h>
#  pragma comment(lib, "advapi32.lib")
#endif /* _WIN32 */

#include <cstddef>
#include <string>
#include <system_error>
#include <vector>

#include "numparse.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

#ifdef _WIN32

	namespace fileowner_detail {

		struct FileHandle {
			HANDLE handle = INVALID_HANDLE_VALUE;

			FileHandle() = default;
			FileHandle(const FileHandle&) = delete;
			FileHandle& operator=(const FileHandle&) = delete;

			~FileHandle() {
				if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
			}
		};

		struct PrivilegeSet {
			DWORD count;
			LUID_AND_ATTRIBUTES privileges[2];
		};

	}  // namespace fileowner_detail

	using fileowner_detail::FileHandle;
	using fileowner_detail::PrivilegeSet;

	static const unsigned long kCurrentUserId = 1000;

	static std::vector<unsigned char> sidBytes(PSID sid) {
		const unsigned char* bytes = static_cast<const unsigned char*>(sid);
		return std::vector<unsigned char>(bytes, bytes + ::GetLengthSid(sid));
	}

	static std::string sidString(PSID sid) {
		LPWSTR text = nullptr;
		if (::ConvertSidToStringSidW(sid, &text) == 0) return "?";

		const std::string result = wideToUtf8(text);
		::LocalFree(text);
		return result;
	}

	static std::string accountName(PSID sid) {
		wchar_t name[256];
		wchar_t domain[256];
		DWORD name_length = 256;
		DWORD domain_length = 256;
		SID_NAME_USE use;
		const BOOL found = ::LookupAccountSidW(nullptr, sid, name, &name_length, domain,
			&domain_length, &use);
		if (found == 0) return sidString(sid);
		return wideToUtf8(std::wstring(name, name_length));
	}

	static bool accountFromSid(PSID sid, Account& account) {
		if (sid == nullptr || ::IsValidSid(sid) == 0) return false;

		account.sid = sidBytes(sid);
		account.name = accountName(sid);
		return true;
	}

	bool currentAccount(Account& account) {
		HANDLE token = nullptr;
		if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return false;

		std::vector<unsigned char> buffer(512);
		DWORD needed = 0;
		const BOOL got = ::GetTokenInformation(token, TokenUser, buffer.data(),
			static_cast<DWORD>(buffer.size()), &needed);
		::CloseHandle(token);
		if (got == 0) return false;

		const TOKEN_USER* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
		return accountFromSid(user->User.Sid, account);
	}

	static bool lookupName(const std::string& text, Account& account) {
		const std::wstring wide = utf8ToWide(text);
		std::vector<unsigned char> sid(SECURITY_MAX_SID_SIZE);
		DWORD sid_size = static_cast<DWORD>(sid.size());
		wchar_t domain[256];
		DWORD domain_length = 256;
		SID_NAME_USE use;
		const BOOL found = ::LookupAccountNameW(nullptr, wide.c_str(), sid.data(), &sid_size,
			domain, &domain_length, &use);
		if (found == 0) return false;

		return accountFromSid(sid.data(), account);
	}

	static bool lookupSidString(const std::string& text, Account& account) {
		if (text.compare(0, 4, "S-1-") != 0) return false;

		PSID sid = nullptr;
		if (::ConvertStringSidToSidW(utf8ToWide(text).c_str(), &sid) == 0) return false;

		const bool ok = accountFromSid(sid, account);
		::LocalFree(sid);
		return ok;
	}

	static bool hasAccount(PSID sid) {
		wchar_t name[256];
		wchar_t domain[256];
		DWORD name_length = 256;
		DWORD domain_length = 256;
		SID_NAME_USE use;
		return ::LookupAccountSidW(nullptr, sid, name, &name_length, domain, &domain_length,
			&use) != 0;
	}

	static bool sidInUserDomain(const Account& user, unsigned long rid, Account& account) {
		std::vector<unsigned char> sid = user.sid;
		PSID candidate = sid.data();
		const UCHAR count = *::GetSidSubAuthorityCount(candidate);
		if (count < 2) return false;

		*::GetSidSubAuthority(candidate, count - 1) = rid;
		if (!hasAccount(candidate)) return false;

		return accountFromSid(candidate, account);
	}

	static bool lookupNumber(const std::string& text, Account& account) {
		const std::string digits = !text.empty() && text[0] == '+' ? text.substr(1) : text;
		unsigned long number = 0;
		std::size_t used = 0;
		if (!parseUL(digits, number, 10, &used) || used != digits.size()) return false;

		Account user;
		if (!currentAccount(user)) return false;
		if (number == kCurrentUserId) {
			account = user;
			return true;
		}

		return sidInUserDomain(user, number, account);
	}

	bool resolveAccount(const std::string& text, Account& account) {
		if (text.empty()) return false;
		if (lookupSidString(text, account)) return true;
		if (text[0] != '+' && lookupName(text, account)) return true;
		return lookupNumber(text, account);
	}

	bool sameAccount(const Account& a, const Account& b) {
		return !a.sid.empty() && a.sid == b.sid;
	}

	static std::error_code lastError() {
		return std::error_code(static_cast<int>(::GetLastError()), std::system_category());
	}

	static std::error_code refusalOrError(DWORD error) {
		switch (error) {
			case ERROR_ACCESS_DENIED:
			case ERROR_PRIVILEGE_NOT_HELD:
			case ERROR_INVALID_OWNER:
			case ERROR_INVALID_PRIMARY_GROUP:
			case ERROR_NOT_SUPPORTED:
				return std::make_error_code(std::errc::operation_not_permitted);
			default:
				return std::error_code(static_cast<int>(error), std::system_category());
		}
	}

	static bool openForSecurity(const fs::path& native, bool follow, DWORD access,
	                            FileHandle& file) {
		const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS
			| (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
		const std::wstring wide = longPathSafe(native).wstring();
		file.handle = ::CreateFileW(wide.c_str(), access,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			flags, nullptr);
		return file.handle != INVALID_HANDLE_VALUE;
	}

	bool readOwnership(const fs::path& native, bool follow, Ownership& ownership,
	                   std::error_code& ec) {
		FileHandle file;
		if (!openForSecurity(native, follow, READ_CONTROL, file)) {
			ec = lastError();
			return false;
		}

		PSID owner = nullptr;
		PSID group = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		const DWORD status = ::GetSecurityInfo(file.handle, SE_FILE_OBJECT,
			OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION, &owner, &group, nullptr,
			nullptr, &descriptor);
		if (status != ERROR_SUCCESS) {
			ec = std::error_code(static_cast<int>(status), std::system_category());
			return false;
		}

		accountFromSid(owner, ownership.owner);
		accountFromSid(group, ownership.group);
		::LocalFree(descriptor);
		return true;
	}

	static std::vector<unsigned char> sidOf(const Account* account) {
		if (account == nullptr) return std::vector<unsigned char>();
		return account->sid;
	}

	bool writeOwnership(const fs::path& native, const OwnershipChange& change,
	                    std::error_code& ec) {
		SECURITY_INFORMATION parts = 0;
		if (change.owner != nullptr) parts |= OWNER_SECURITY_INFORMATION;
		if (change.group != nullptr) parts |= GROUP_SECURITY_INFORMATION;
		if (parts == 0) return true;

		FileHandle file;
		if (!openForSecurity(native, change.follow, WRITE_OWNER | READ_CONTROL, file)) {
			ec = refusalOrError(::GetLastError());
			return false;
		}

		std::vector<unsigned char> owner_sid = sidOf(change.owner);
		std::vector<unsigned char> group_sid = sidOf(change.group);
		PSID owner = owner_sid.empty() ? nullptr : owner_sid.data();
		PSID group = group_sid.empty() ? nullptr : group_sid.data();
		const DWORD status = ::SetSecurityInfo(file.handle, SE_FILE_OBJECT, parts, owner, group,
			nullptr, nullptr);
		if (status == ERROR_SUCCESS) return true;

		ec = refusalOrError(status);
		return false;
	}

	static bool addPrivilege(const wchar_t* name, PrivilegeSet& set) {
		LUID luid;
		if (::LookupPrivilegeValueW(nullptr, name, &luid) == 0) return false;

		set.privileges[set.count].Luid = luid;
		set.privileges[set.count].Attributes = SE_PRIVILEGE_ENABLED;
		++set.count;
		return true;
	}

	OwnershipPrivileges::OwnershipPrivileges() {
		HANDLE token = nullptr;
		const DWORD access = TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY;
		if (::OpenProcessToken(::GetCurrentProcess(), access, &token) == 0) return;

		PrivilegeSet wanted{};
		addPrivilege(SE_RESTORE_NAME, wanted);
		addPrivilege(SE_TAKE_OWNERSHIP_NAME, wanted);

		previous_.resize(sizeof(PrivilegeSet));
		DWORD previous_size = static_cast<DWORD>(previous_.size());
		const BOOL adjusted = ::AdjustTokenPrivileges(token, FALSE,
			reinterpret_cast<TOKEN_PRIVILEGES*>(&wanted), previous_size,
			reinterpret_cast<TOKEN_PRIVILEGES*>(previous_.data()), &previous_size);
		if (adjusted == 0) {
			::CloseHandle(token);
			previous_.clear();
			return;
		}

		token_ = token;
	}

	OwnershipPrivileges::~OwnershipPrivileges() {
		if (token_ == nullptr) return;

		::AdjustTokenPrivileges(static_cast<HANDLE>(token_), FALSE,
			reinterpret_cast<TOKEN_PRIVILEGES*>(previous_.data()), 0, nullptr, nullptr);
		::CloseHandle(static_cast<HANDLE>(token_));
	}

#else

	bool resolveAccount(const std::string&, Account&) { return false; }

	bool currentAccount(Account&) { return false; }

	bool sameAccount(const Account& a, const Account& b) { return a.sid == b.sid; }

	bool readOwnership(const fs::path&, bool, Ownership&, std::error_code& ec) {
		ec = std::make_error_code(std::errc::not_supported);
		return false;
	}

	bool writeOwnership(const fs::path&, const OwnershipChange&, std::error_code& ec) {
		ec = std::make_error_code(std::errc::operation_not_permitted);
		return false;
	}

	OwnershipPrivileges::OwnershipPrivileges() {}

	OwnershipPrivileges::~OwnershipPrivileges() {}

#endif /* _WIN32 */

}  // namespace wbsh
