/**
 * @file session.cpp
 * @brief Reader thread, wake-ups, and the parse step.
 */

#include "session.h"

#include <cstdio>

namespace wbshterm {

	static const DWORD kReadChunk = 256 * 1024;

	// A frame from the pseudoconsole can arrive as several writes a
	// moment apart. Painting between them shows half of it, so a read
	// waits this long for the rest before the window hears -- but never
	// holds bytes back beyond the limit, or typing would start to lag.
	static const long long kGatherGraceMicroseconds = 2000;
	static const long long kGatherLimitMilliseconds = 8;

	static HANDLE createGatherTimer() {
		return ::CreateWaitableTimerExW(nullptr, nullptr,
			CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	}

	Session::Session() {
		screen_.setResponder(this);
	}

	Session::~Session() {
		stop();
	}

	bool Session::start(const ShellCommand& shell, int columns, int rows,
			std::string& out_error) {
		screen_.resize(columns, rows);

		PtySize size;
		size.columns = static_cast<SHORT>(columns);
		size.rows    = static_cast<SHORT>(rows);
		if (!pty_.open(shell, size, out_error)) return false;

		stop_signal_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		reader_ = std::thread(&Session::readLoop, this);
		child_watch_ = std::thread(&Session::watchChildLoop, this);
		return true;
	}

	void Session::wakeWith(HWND window, UINT message) {
		wake_window_  = window;
		wake_message_ = message;
	}

	void Session::readLoop() {
		std::vector<char> buffer(kReadChunk);
		gather_timer_ = createGatherTimer();

		for (;;) {
			const DWORD got = pty_.read(buffer.data(), kReadChunk);
			if (got == 0) break;

			appendBytes(buffer.data(), got);
			if (moreOfFrameComing(got, kReadChunk)) continue;

			notifyWindow();
		}

		notifyWindow();
		if (gather_timer_ != nullptr) ::CloseHandle(gather_timer_);
		gather_timer_ = nullptr;
	}

	bool Session::moreOfFrameComing(DWORD read_size, DWORD capacity) {
		const auto now = std::chrono::steady_clock::now();
		if (!gathering_) {
			gathering_ = true;
			gather_started_ = now;
		}

		const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
			now - gather_started_).count();
		if (held >= kGatherLimitMilliseconds) {
			gathering_ = false;
			return false;
		}

		if (read_size == capacity || pty_.bytesAvailable() > 0) return true;

		waitForMoreOutput();
		if (pty_.bytesAvailable() > 0) return true;

		gathering_ = false;
		return false;
	}

	// The default Sleep granularity is a whole video frame, which is far
	// more than the pause wanted here; a high-resolution timer waits for
	// as little as asked. Without one the wait is skipped rather than
	// taken at the coarse length.
	void Session::waitForMoreOutput() {
		if (gather_timer_ == nullptr) return;

		LARGE_INTEGER due{};
		due.QuadPart = -kGatherGraceMicroseconds * 10;
		if (::SetWaitableTimer(gather_timer_, &due, 0, nullptr, nullptr, FALSE) == 0) return;

		::WaitForSingleObject(gather_timer_, 5);
	}

	void Session::watchChildLoop() {
		const HANDLE child = pty_.childHandle();
		if (child == INVALID_HANDLE_VALUE || stop_signal_ == nullptr) return;

		const HANDLE waited[2] = { child, stop_signal_ };
		::WaitForMultipleObjects(2, waited, FALSE, INFINITE);

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

		if (stop_signal_ != nullptr) ::SetEvent(stop_signal_);
		if (child_watch_.joinable()) child_watch_.join();

		pty_.endSession();
		if (reader_.joinable()) reader_.join();
		pty_.close();

		if (stop_signal_ != nullptr) {
			::CloseHandle(stop_signal_);
			stop_signal_ = nullptr;
		}
	}

} /* namespace wbshterm */
