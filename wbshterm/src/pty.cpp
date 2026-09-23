/**
 * @file pty.cpp
 * @brief ConPTY session setup, teardown, and byte-level I/O.
 */

#include "pty.h"

#include <cstdio>
#include <vector>

namespace wbshterm {

	static std::string describeLastError(const char* call) {
		const DWORD code = ::GetLastError();
		char text[160];
		std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", call, code);
		return text;
	}

	static void closeIfOpen(HANDLE& handle) {
		if (handle != INVALID_HANDLE_VALUE && handle != nullptr) ::CloseHandle(handle);
		handle = INVALID_HANDLE_VALUE;
	}

	PtySession::~PtySession() {
		close();
	}

	bool PtySession::open(const std::wstring& command_line, PtySize size, std::string& out_error) {
		if (!createPipesAndConsole(size, out_error)) {
			close();
			return false;
		}

		if (!spawnChild(command_line, out_error)) {
			close();
			return false;
		}

		return true;
	}

	// The pseudoconsole duplicates the two handles it is given, so the
	// originals are closed here; leaving them open keeps the child's
	// stdin alive forever and read() would never see end of stream.
	bool PtySession::createPipesAndConsole(PtySize size, std::string& out_error) {
		HANDLE input_read   = INVALID_HANDLE_VALUE;
		HANDLE output_write = INVALID_HANDLE_VALUE;

		if (!::CreatePipe(&input_read, &input_write_, nullptr, 0)) {
			out_error = describeLastError("CreatePipe(input)");
			return false;
		}

		if (!::CreatePipe(&output_read_, &output_write, nullptr, 0)) {
			out_error = describeLastError("CreatePipe(output)");
			closeIfOpen(input_read);
			return false;
		}

		const COORD extent = { size.columns, size.rows };
		const HRESULT hr = ::CreatePseudoConsole(extent, input_read, output_write, 0, &console_);

		closeIfOpen(input_read);
		closeIfOpen(output_write);

		if (FAILED(hr)) {
			char text[160];
			std::snprintf(text, sizeof(text), "CreatePseudoConsole failed (0x%08lX)",
				static_cast<unsigned long>(hr));
			out_error = text;
			return false;
		}

		return true;
	}

	bool PtySession::spawnChild(const std::wstring& command_line, std::string& out_error) {
		SIZE_T attribute_bytes = 0;
		::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);

		std::vector<char> attribute_storage(attribute_bytes);
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
		if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
			out_error = describeLastError("InitializeProcThreadAttributeList");
			return false;
		}

		if (!::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
				console_, sizeof(console_), nullptr, nullptr)) {
			out_error = describeLastError("UpdateProcThreadAttribute");
			::DeleteProcThreadAttributeList(attributes);
			return false;
		}

		// Null std handles with STARTF_USESTDHANDLES are what make the child
		// adopt the pseudoconsole's handles. Without them this process's own
		// stdout reaches the child, which then writes past the pty entirely:
		// its console is the pty (GetConsoleScreenBufferInfo sees the right
		// size) while printf lands in our stdout. The SDK's EchoCon sample
		// omits this; on Windows 10 22H2 it is required.
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = nullptr;
		startup.StartupInfo.hStdOutput = nullptr;
		startup.StartupInfo.hStdError = nullptr;
		startup.lpAttributeList = attributes;

		std::wstring mutable_command_line = command_line;
		PROCESS_INFORMATION child{};
		const BOOL spawned = ::CreateProcessW(nullptr, mutable_command_line.data(),
			nullptr, nullptr, FALSE,
			EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
			nullptr, nullptr, &startup.StartupInfo, &child);

		::DeleteProcThreadAttributeList(attributes);

		if (!spawned) {
			out_error = describeLastError("CreateProcess");
			return false;
		}

		::CloseHandle(child.hThread);
		child_ = child.hProcess;
		return true;
	}

	bool PtySession::resize(PtySize size) {
		if (console_ == nullptr) return false;

		const COORD extent = { size.columns, size.rows };
		return SUCCEEDED(::ResizePseudoConsole(console_, extent));
	}

	DWORD PtySession::read(char* buffer, DWORD capacity) {
		if (output_read_ == INVALID_HANDLE_VALUE) return 0;

		DWORD got = 0;
		if (!::ReadFile(output_read_, buffer, capacity, &got, nullptr)) return 0;
		return got;
	}

	bool PtySession::write(const char* data, DWORD length) {
		if (input_write_ == INVALID_HANDLE_VALUE) return false;

		DWORD written = 0;
		while (written < length) {
			DWORD chunk = 0;
			if (!::WriteFile(input_write_, data + written, length - written, &chunk, nullptr)) {
				return false;
			}

			if (chunk == 0) return false;
			written += chunk;
		}

		return true;
	}

	bool PtySession::waitForExit(DWORD timeout_ms, DWORD& out_exit_code) {
		if (child_ == INVALID_HANDLE_VALUE) return false;
		if (::WaitForSingleObject(child_, timeout_ms) != WAIT_OBJECT_0) return false;

		return ::GetExitCodeProcess(child_, &out_exit_code) != 0;
	}

	bool PtySession::childAlive() const {
		if (child_ == INVALID_HANDLE_VALUE) return false;
		return ::WaitForSingleObject(child_, 0) == WAIT_TIMEOUT;
	}

	void PtySession::endSession() {
		closeIfOpen(input_write_);

		if (console_ != nullptr) {
			::ClosePseudoConsole(console_);
			console_ = nullptr;
		}
	}

	void PtySession::close() {
		endSession();
		closeIfOpen(output_read_);
		closeIfOpen(child_);
	}

} /* namespace wbshterm */
