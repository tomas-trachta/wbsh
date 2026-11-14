/**
 * @file jsonrun.cpp
 * @brief `wbsh --json`: one run, its streams captured, reported as JSON.
 */

#include "jsonrun.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>

#include "utf8.h"

namespace wbsh {

	namespace jsonrun_detail {

		struct CaptureFile {
			HANDLE reader    = INVALID_HANDLE_VALUE;
			int    writer_fd = -1;
		};

		struct StreamCapture {
			CaptureFile out;
			CaptureFile err;
			HANDLE      real_stdout = INVALID_HANDLE_VALUE;
		};

		struct RunResult {
			int         status = 0;
			std::string out;
			std::string err;
			long long   duration_ms = 0;
		};

	}  // namespace jsonrun_detail

	using jsonrun_detail::CaptureFile;
	using jsonrun_detail::RunResult;
	using jsonrun_detail::StreamCapture;

	static const DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	static const char  kHexDigits[] = "0123456789abcdef";
	static const DWORD kMaxWriteChunk = 1u << 20;

	static bool tempFilePath(std::wstring& out_path) {
		wchar_t dir[MAX_PATH];
		const DWORD dir_length = ::GetTempPathW(MAX_PATH, dir);
		if (dir_length == 0 || dir_length > MAX_PATH) return false;

		wchar_t path[MAX_PATH];
		if (::GetTempFileNameW(dir, L"wbj", 0, path) == 0) return false;

		out_path = path;
		return true;
	}

	// Delete-on-close removes the file with its last handle, so a run that is
	// killed, or leaves a background job writing, leaves nothing behind.
	static HANDLE openCaptureHandle(const std::wstring& path, DWORD access, DWORD disposition) {
		return ::CreateFileW(path.c_str(), access, kShareAll, nullptr, disposition,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
	}

	static bool openCaptureFile(CaptureFile& out_file) {
		std::wstring path;
		if (!tempFilePath(path)) return false;

		const HANDLE writer = openCaptureHandle(path, GENERIC_WRITE, CREATE_ALWAYS);
		if (writer == INVALID_HANDLE_VALUE) {
			::DeleteFileW(path.c_str());
			return false;
		}

		out_file.reader = openCaptureHandle(path, GENERIC_READ, OPEN_EXISTING);
		const intptr_t os_handle = reinterpret_cast<intptr_t>(writer);
		out_file.writer_fd = _open_osfhandle(os_handle, _O_WRONLY | _O_BINARY);
		if (out_file.writer_fd < 0) ::CloseHandle(writer);

		return out_file.reader != INVALID_HANDLE_VALUE && out_file.writer_fd >= 0;
	}

	// A copy outside the CRT's fd table: nothing the command does to its
	// descriptors (`exec 1>&-`, `exec 12>file`) can reach it.
	static bool duplicateRealStdout(HANDLE& out_handle) {
		const HANDLE real = reinterpret_cast<HANDLE>(_get_osfhandle(1));
		if (real == INVALID_HANDLE_VALUE) return false;

		const HANDLE self = ::GetCurrentProcess();
		const DWORD options = DUPLICATE_SAME_ACCESS;
		return ::DuplicateHandle(self, real, self, &out_handle, 0, FALSE, options) != 0;
	}

	static void pointFdAtCapture(int fd, CaptureFile& file) {
		_dup2(file.writer_fd, fd);
		_close(file.writer_fd);
		file.writer_fd = -1;
	}

	static bool beginCapture(StreamCapture& capture) {
		if (!openCaptureFile(capture.out) || !openCaptureFile(capture.err)) return false;
		if (!duplicateRealStdout(capture.real_stdout)) return false;

		std::cout.flush();
		std::fflush(stdout);
		std::fflush(stderr);
		pointFdAtCapture(1, capture.out);
		pointFdAtCapture(2, capture.err);
		return true;
	}

	static std::string readCapture(HANDLE reader) {
		std::string text;
		char buffer[65536];
		DWORD got = 0;
		while (::ReadFile(reader, buffer, sizeof buffer, &got, nullptr) != 0 && got > 0) {
			text.append(buffer, got);
		}

		return text;
	}

	static void endCapture(const StreamCapture& capture, RunResult& result) {
		std::cout.flush();
		std::cerr.flush();
		std::fflush(stdout);
		std::fflush(stderr);

		result.out = readCapture(capture.out.reader);
		result.err = readCapture(capture.err.reader);
	}

	static void appendCodeUnit(std::string& out, unsigned unit) {
		out += "\\u";
		for (int shift = 12; shift >= 0; shift -= 4) out += kHexDigits[(unit >> shift) & 0xF];
	}

	static void appendNonAscii(std::string& out, char32_t code) {
		if (code < 0x10000) {
			appendCodeUnit(out, static_cast<unsigned>(code));
			return;
		}

		const char32_t offset = code - 0x10000;
		appendCodeUnit(out, 0xD800 + static_cast<unsigned>(offset >> 10));
		appendCodeUnit(out, 0xDC00 + static_cast<unsigned>(offset & 0x3FF));
	}

	static const char* shortEscape(char c) {
		switch (c) {
		case '"':
			return "\\\"";
		case '\\':
			return "\\\\";
		case '\n':
			return "\\n";
		case '\r':
			return "\\r";
		case '\t':
			return "\\t";
		default:
			return nullptr;
		}
	}

	static void appendAscii(std::string& out, char c) {
		const char* escape = shortEscape(c);
		if (escape != nullptr) out += escape;
		else if (c < 0x20 || c == 0x7F) appendCodeUnit(out, static_cast<unsigned>(c));
		else out += c;
	}

	// decodeUtf8 hands back a stray byte as 0xDC00 + byte; any surrogate
	// here is therefore bytes that were not UTF-8.
	static bool isUnicodeScalar(char32_t code) {
		return code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF);
	}

	static void appendJsonString(std::string& out, const std::string& bytes) {
		out += '"';
		std::size_t pos = 0;
		while (pos < bytes.size()) {
			const Utf8Char ch = decodeUtf8(bytes, pos);
			if (ch.code < 0x80) appendAscii(out, static_cast<char>(ch.code));
			else if (isUnicodeScalar(ch.code)) appendNonAscii(out, ch.code);
			else appendCodeUnit(out, 0xFFFD);

			pos += ch.length;
		}

		out += '"';
	}

	static std::string resultJson(const RunResult& result) {
		std::string json = "{\"status\":" + std::to_string(result.status) + ",\"stdout\":";
		appendJsonString(json, result.out);
		json += ",\"stderr\":";
		appendJsonString(json, result.err);
		json += ",\"duration_ms\":" + std::to_string(result.duration_ms) + "}\n";
		return json;
	}

	static void writeAll(HANDLE handle, const std::string& text) {
		std::size_t done = 0;
		while (done < text.size()) {
			const std::size_t left = text.size() - done;
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(left, kMaxWriteChunk));
			DWORD wrote = 0;
			const BOOL ok = ::WriteFile(handle, text.data() + done, chunk, &wrote, nullptr);
			if (ok == 0 || wrote == 0) return;

			done += wrote;
		}
	}

	static long long millisecondsSince(std::chrono::steady_clock::time_point started) {
		const auto elapsed = std::chrono::steady_clock::now() - started;
		return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	}

	int runCapturedAsJson(const std::function<int()>& body) {
		StreamCapture capture;
		if (!beginCapture(capture)) {
			std::cerr << "wbsh: --json: cannot create the capture files\n";
			return 1;
		}

		RunResult result;
		const auto started = std::chrono::steady_clock::now();
		result.status = body();
		result.duration_ms = millisecondsSince(started);

		endCapture(capture, result);
		writeAll(capture.real_stdout, resultJson(result));
		return result.status;
	}

}  // namespace wbsh
