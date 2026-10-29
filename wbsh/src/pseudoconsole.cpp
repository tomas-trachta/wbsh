/**
 * @file pseudoconsole.cpp
 * @brief Pseudoconsole creation, resizing, teardown and byte-level I/O.
 */

#include "pseudoconsole.h"

#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	namespace pseudoconsole_detail {

		typedef HRESULT (WINAPI *CreateConsoleFn)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
		typedef HRESULT (WINAPI *ResizeConsoleFn)(HPCON, COORD);
		typedef void    (WINAPI *CloseConsoleFn)(HPCON);

		struct ConsoleApi {
			CreateConsoleFn create = nullptr;
			ResizeConsoleFn resize = nullptr;
			CloseConsoleFn  close  = nullptr;
		};

	}  // namespace pseudoconsole_detail

	using pseudoconsole_detail::ConsoleApi;
	using pseudoconsole_detail::CloseConsoleFn;
	using pseudoconsole_detail::CreateConsoleFn;
	using pseudoconsole_detail::ResizeConsoleFn;

	// A pseudoconsole writes a whole frame at once when the pipe has room
	// for it; with the default few kilobytes a busy screen dribbles out.
	static const DWORD kOutputPipeBytes = 1024 * 1024;
	static const DWORD kLongPathChars = 32768;

	static std::wstring directoryOfThisExe() {
		std::vector<wchar_t> path(kLongPathChars);
		const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), kLongPathChars);
		const std::wstring full(path.data(), length);
		const std::size_t slash = full.find_last_of(L"\\/");
		return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1);
	}

	// Windows 10 never updates its own pseudoconsole, and the 2019 one in
	// it turns a busy program into a slow trickle and repaints on resize.
	static ConsoleApi loadBundledConsole() {
		ConsoleApi api;
		const std::wstring path = directoryOfThisExe() + L"conpty.dll";
		const HMODULE library = ::LoadLibraryExW(path.c_str(), nullptr,
			LOAD_WITH_ALTERED_SEARCH_PATH);
		if (library == nullptr) return api;

		api.create = reinterpret_cast<CreateConsoleFn>(
			::GetProcAddress(library, "ConptyCreatePseudoConsole"));
		api.resize = reinterpret_cast<ResizeConsoleFn>(
			::GetProcAddress(library, "ConptyResizePseudoConsole"));
		api.close = reinterpret_cast<CloseConsoleFn>(
			::GetProcAddress(library, "ConptyClosePseudoConsole"));
		if (api.create != nullptr && api.resize != nullptr && api.close != nullptr) return api;

		::FreeLibrary(library);
		return ConsoleApi();
	}

	static ConsoleApi systemConsole() {
		ConsoleApi api;
		api.create = &::CreatePseudoConsole;
		api.resize = &::ResizePseudoConsole;
		api.close  = &::ClosePseudoConsole;
		return api;
	}

	static ConsoleApi chooseConsoleApi() {
		const ConsoleApi bundled = loadBundledConsole();
		return bundled.create != nullptr ? bundled : systemConsole();
	}

	static const ConsoleApi& consoleApi() {
		static const ConsoleApi api = chooseConsoleApi();
		return api;
	}

	static std::string describeFailure(const char* call, unsigned long code) {
		char text[160];
		std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", call, code);
		return text;
	}

	static void closeIfOpen(HANDLE& handle) {
		if (handle != INVALID_HANDLE_VALUE && handle != nullptr) ::CloseHandle(handle);
		handle = INVALID_HANDLE_VALUE;
	}

	bool PseudoConsole::usesBundledConsole() {
		return consoleApi().create != &::CreatePseudoConsole;
	}

	PseudoConsole::~PseudoConsole() {
		close();
	}

	// The pseudoconsole duplicates the two handles it is given, so its ends
	// are closed here; kept open, the child's input would never see its
	// end and read() would never return 0.
	bool PseudoConsole::open(PseudoConsoleSize size, std::string& out_error) {
		HANDLE input_read   = INVALID_HANDLE_VALUE;
		HANDLE output_write = INVALID_HANDLE_VALUE;
		if (::CreatePipe(&input_read, &input_write_, nullptr, 0) == 0) {
			out_error = describeFailure("CreatePipe", ::GetLastError());
			return false;
		}

		if (::CreatePipe(&output_read_, &output_write, nullptr, kOutputPipeBytes) == 0) {
			out_error = describeFailure("CreatePipe", ::GetLastError());
			closeIfOpen(input_read);
			close();
			return false;
		}

		const COORD extent = { size.columns, size.rows };
		const HRESULT hr = consoleApi().create(extent, input_read, output_write, 0, &console_);
		closeIfOpen(input_read);
		closeIfOpen(output_write);
		if (SUCCEEDED(hr)) return true;

		console_ = nullptr;
		out_error = describeFailure("CreatePseudoConsole", static_cast<unsigned long>(hr));
		close();
		return false;
	}

	bool PseudoConsole::resize(PseudoConsoleSize size) {
		if (console_ == nullptr) return false;

		const COORD extent = { size.columns, size.rows };
		return SUCCEEDED(consoleApi().resize(console_, extent));
	}

	DWORD PseudoConsole::read(char* buffer, DWORD capacity) {
		if (output_read_ == INVALID_HANDLE_VALUE) return 0;

		DWORD got = 0;
		if (::ReadFile(output_read_, buffer, capacity, &got, nullptr) == 0) return 0;
		return got;
	}

	bool PseudoConsole::write(const char* data, DWORD length) {
		if (input_write_ == INVALID_HANDLE_VALUE) return false;

		DWORD written = 0;
		while (written < length) {
			DWORD chunk = 0;
			if (::WriteFile(input_write_, data + written, length - written, &chunk, nullptr) == 0) {
				return false;
			}

			if (chunk == 0) return false;
			written += chunk;
		}

		return true;
	}

	void PseudoConsole::endSession() {
		if (console_ != nullptr) {
			consoleApi().close(console_);
			console_ = nullptr;
		}

		closeIfOpen(input_write_);
	}

	void PseudoConsole::close() {
		endSession();
		closeIfOpen(output_read_);
	}

}  // namespace wbsh
