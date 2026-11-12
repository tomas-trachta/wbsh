/**
 * @file pipesecurity.cpp
 * @brief Security attributes for the named pipes wbsh creates.
 */

#include "pipesecurity.h"

#ifdef _WIN32

#include <sddl.h>

#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace wbsh {

	static std::wstring currentUserSidText() {
		HANDLE token = nullptr;
		if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) return {};

		DWORD size = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<BYTE> buffer(size);
		std::wstring text;
		if (size != 0 && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
			const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
			LPWSTR sid = nullptr;
			if (::ConvertSidToStringSidW(user->User.Sid, &sid) != 0) {
				text = sid;
				::LocalFree(sid);
			}
		}

		::CloseHandle(token);
		return text;
	}

	SECURITY_ATTRIBUTES* currentUserPipeSecurity() {
		static SECURITY_ATTRIBUTES attributes{};
		static const bool ready = [] {
			const std::wstring sid = currentUserSidText();
			if (sid.empty()) return false;

			// Protected DACL: full access for the user and SYSTEM, nobody else.
			const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;SY)";
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
					SDDL_REVISION_1, &descriptor, nullptr) == 0) {
				return false;
			}

			attributes.nLength = sizeof(attributes);
			attributes.lpSecurityDescriptor = descriptor;
			attributes.bInheritHandle = FALSE;
			return true;
		}();

		return ready ? &attributes : nullptr;
	}

}  // namespace wbsh

#endif  // _WIN32
