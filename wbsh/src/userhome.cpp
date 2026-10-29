/**
 * @file userhome.cpp
 * @brief Account lookup and the ProfileList registry behind `~name`.
 */

#include "userhome.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <userenv.h>
#pragma comment(lib, "userenv.lib")

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "pathconv.h"

namespace wbsh {

	static const wchar_t kProfileList[] =
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
	static const wchar_t kLocalUserSidPrefix[] = L"S-1-5-21-";

	static bool userSid(const std::wstring& name, std::vector<unsigned char>& sid) {
		DWORD sid_bytes = 0;
		DWORD domain_chars = 0;
		SID_NAME_USE use = SidTypeUnknown;
		::LookupAccountNameW(nullptr, name.c_str(), nullptr, &sid_bytes, nullptr, &domain_chars, &use);
		if (sid_bytes == 0) return false;

		sid.resize(sid_bytes);
		std::wstring domain(domain_chars, L'\0');
		const BOOL found = ::LookupAccountNameW(nullptr, name.c_str(), sid.data(), &sid_bytes,
			domain.data(), &domain_chars, &use);
		return found != 0 && use == SidTypeUser;
	}

	// REG_EXPAND_SZ values come back with their %VARIABLES% expanded.
	static LSTATUS readRegistryText(const std::wstring& key, const wchar_t* value, void* data,
	                               DWORD& bytes) {
		const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ;
		return ::RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, flags, nullptr, data, &bytes);
	}

	static std::wstring registryText(const std::wstring& key, const wchar_t* value) {
		DWORD bytes = 0;
		if (readRegistryText(key, value, nullptr, bytes) != ERROR_SUCCESS) return std::wstring();

		std::wstring text(bytes / sizeof(wchar_t) + 1, L'\0');
		if (readRegistryText(key, value, text.data(), bytes) != ERROR_SUCCESS) return std::wstring();

		text.resize(::wcsnlen(text.c_str(), text.size()));
		return text;
	}

	static std::wstring sidText(std::vector<unsigned char> sid) {
		LPWSTR text = nullptr;
		if (::ConvertSidToStringSidW(sid.data(), &text) == 0) {
			return std::wstring();
		}

		const std::wstring copy = text;
		::LocalFree(text);
		return copy;
	}

	static std::wstring registeredProfile(const std::vector<unsigned char>& sid) {
		const std::wstring text = sidText(sid);
		if (text.empty()) return std::wstring();
		return registryText(std::wstring(kProfileList) + L"\\" + text, L"ProfileImagePath");
	}

	// Where Windows would put the profile on the account's first logon.
	static std::wstring defaultProfile(const std::wstring& name) {
		DWORD chars = 0;
		::GetProfilesDirectoryW(nullptr, &chars);
		std::wstring directory(chars, L'\0');
		if (chars == 0 || ::GetProfilesDirectoryW(directory.data(), &chars) == 0) {
			return std::wstring();
		}

		directory.resize(::wcsnlen(directory.c_str(), directory.size()));
		return directory + L"\\" + name;
	}

	static std::wstring lookUpHome(const std::wstring& name) {
		std::vector<unsigned char> sid;
		if (!userSid(name, sid)) return std::wstring();

		const std::wstring registered = registeredProfile(sid);
		return registered.empty() ? defaultProfile(name) : registered;
	}

	static std::wstring lowered(std::wstring text) {
		for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
		return text;
	}

	// An unknown name may cost a round trip to a domain controller, so
	// misses are remembered as well.
	static std::wstring cachedHome(const std::wstring& name) {
		static std::mutex lock;
		static std::map<std::wstring, std::wstring> homes;

		const std::wstring key = lowered(name);
		const std::lock_guard<std::mutex> guard(lock);
		const auto known = homes.find(key);
		if (known != homes.end()) return known->second;

		const std::wstring home = lookUpHome(name);
		homes.emplace(key, home);
		return home;
	}

	std::string userHomeDirectory(const PathConv& paths, const std::string& user) {
		if (user.empty()) return std::string();

		const std::wstring home = cachedHome(utf8ToWide(user));
		return home.empty() ? std::string() : paths.toPosix(wideToUtf8(home));
	}

	static std::wstring accountName(const std::wstring& sid_text) {
		PSID sid = nullptr;
		if (::ConvertStringSidToSidW(sid_text.c_str(), &sid) == 0) return std::wstring();

		wchar_t name[256];
		wchar_t domain[256];
		DWORD name_chars = 256;
		DWORD domain_chars = 256;
		SID_NAME_USE use = SidTypeUnknown;
		const BOOL found = ::LookupAccountSidW(nullptr, sid, name, &name_chars, domain,
			&domain_chars, &use);
		::LocalFree(sid);
		return found != 0 && use == SidTypeUser ? std::wstring(name, name_chars) : std::wstring();
	}

	static std::vector<std::wstring> profileSids() {
		std::vector<std::wstring> sids;
		HKEY list = nullptr;
		const LSTATUS opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProfileList, 0, KEY_READ, &list);
		if (opened != ERROR_SUCCESS) return sids;

		wchar_t subkey[256];
		for (DWORD index = 0;; ++index) {
			DWORD chars = 256;
			if (::RegEnumKeyExW(list, index, subkey, &chars, nullptr, nullptr, nullptr, nullptr)
					!= ERROR_SUCCESS) {
				break;
			}

			if (std::wcsncmp(subkey, kLocalUserSidPrefix, std::wcslen(kLocalUserSidPrefix)) == 0) {
				sids.emplace_back(subkey, chars);
			}
		}

		::RegCloseKey(list);
		return sids;
	}

	std::vector<std::string> profileUserNames() {
		std::vector<std::string> names;
		for (const std::wstring& sid : profileSids()) {
			const std::wstring name = accountName(sid);
			if (!name.empty()) names.push_back(wideToUtf8(name));
		}

		std::sort(names.begin(), names.end());
		names.erase(std::unique(names.begin(), names.end()), names.end());
		return names;
	}

}  // namespace wbsh
