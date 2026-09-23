/**
 * @file session.cpp
 * @brief Reader thread, wake-ups, and the parse step.
 */

#include "session.h"

#include <cstdio>

namespace wbshterm {

	static const DWORD kReadChunk = 16384;

	Session::Session() {
		screen_.setResponder(this);
	}

	Session::~Session() {
		stop();
	}

	bool Session::start(const std::wstring& command_line, int columns, int rows,
			std::string& out_error) {
		screen_.resize(columns, rows);

		PtySize size;
		size.columns = static_cast<SHORT>(columns);
		size.rows    = static_cast<SHORT>(rows);
		if (!pty_.open(command_line, size, out_error)) return false;

		reader_ = std::thread(&Session::readLoop, this);
		return true;
	}

	void Session::wakeWith(HWND window, UINT message) {
		wake_window_  = window;
		wake_message_ = message;
	}

	void Session::readLoop() {
		std::vector<char> buffer(kReadChunk);

		for (;;) {
			const DWORD got = pty_.read(buffer.data(), kReadChunk);
			if (got == 0) break;

			appendBytes(buffer.data(), got);
			notifyWindow();
		}

		notifyWindow();
	}

	void Session::appendBytes(const char* data, std::size_t length) {
		std::lock_guard<std::mutex> guard(buffer_lock_);
		pending_.insert(pending_.end(), data, data + length);
	}

	std::vector<char> Session::takePending() {
		std::vector<char> chunk;

		std::lock_guard<std::mutex> guard(buffer_lock_);
		chunk.swap(pending_);
		return chunk;
	}

	// One posted message at a time: a flood of small reads must not fill
	// the message queue, and the UI thread drains everything buffered.
	void Session::notifyWindow() {
		if (wake_window_ == nullptr) return;
		if (wake_posted_.exchange(true)) return;

		::PostMessageW(wake_window_, wake_message_, 0, 0);
	}

	void Session::recordChunk(const std::vector<char>& chunk) {
		if (record_path_.empty()) return;

		FILE* file = nullptr;
		if (_wfopen_s(&file, record_path_.c_str(), L"ab") != 0 || file == nullptr) return;

		std::fwrite(chunk.data(), 1, chunk.size(), file);
		std::fclose(file);
	}

	bool Session::drainOutput() {
		wake_posted_.store(false);

		const std::vector<char> chunk = takePending();
		if (chunk.empty()) return false;

		recordChunk(chunk);
		parser_.consume(chunk.data(), chunk.size());
		return true;
	}

	void Session::resize(int columns, int rows) {
		screen_.resize(columns, rows);

		PtySize size;
		size.columns = static_cast<SHORT>(columns);
		size.rows    = static_cast<SHORT>(rows);
		pty_.resize(size);
	}

	void Session::writeInput(const char* data, std::size_t length) {
		pty_.write(data, static_cast<DWORD>(length));
	}

	void Session::vtRespond(const std::string& bytes) {
		writeInput(bytes.data(), bytes.size());
	}

	bool Session::childRunning() const {
		return pty_.childAlive();
	}

	void Session::stop() {
		if (stopping_.exchange(true)) return;

		pty_.endSession();
		if (reader_.joinable()) reader_.join();
		pty_.close();
	}

} /* namespace wbshterm */
