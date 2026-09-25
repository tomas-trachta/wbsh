#pragma once

/**
 * @file session.h
 * @brief A shell on a pseudoconsole, parsed into a screen.
 */

#include "pty.h"
#include "screen.h"
#include "vtparse.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wbshterm {

	/**
	 * @brief Owns the pty, the reader thread, and the grid it feeds.
	 *
	 * The reader thread only appends bytes to a buffer. Parsing and every
	 * read of the screen happen on the thread that calls drainOutput(),
	 * so the grid itself needs no locking.
	 */
	class Session : public VtResponder {
	public:
		Session();
		~Session() override;

		Session(const Session&)            = delete;
		Session& operator=(const Session&) = delete;

		bool start(const ShellCommand& shell, int columns, int rows,
			std::string& out_error);

		/** Wakes this window with this message when bytes arrive. */
		void wakeWith(HWND window, UINT message);

		void resize(int columns, int rows);
		void writeInput(const char* data, std::size_t length);
		void vtRespond(const std::string& bytes) override;

		/** Parses whatever has arrived. Returns true if the screen changed. */
		bool drainOutput();

		/** Appends every byte parsed from here on to @p path, for goldens. */
		void recordTo(const std::wstring& path) { record_path_ = path; }

		bool childRunning() const;
		void stop();

		Screen& screen() { return screen_; }
		const Screen& screen() const { return screen_; }

	private:
		void readLoop();
		bool moreOfFrameComing(DWORD read_size, DWORD capacity);
		void waitForMoreOutput();
		void watchChildLoop();
		void appendBytes(const char* data, std::size_t length);
		std::vector<char> takePending();
		void recordChunk(const std::vector<char>& chunk);
		void notifyWindow();

		PtySession pty_;
		Screen     screen_{ 80, 24 };
		VtParser   parser_{ screen_ };

		std::thread       reader_;
		std::thread       child_watch_;
		HANDLE            stop_signal_ = nullptr;
		std::mutex        buffer_lock_;
		std::vector<char> pending_;
		std::atomic<bool> stopping_{ false };
		std::atomic<bool> wake_posted_{ false };
		HANDLE            gather_timer_ = nullptr;
		bool              gathering_ = false;
		std::chrono::steady_clock::time_point gather_started_;

		std::wstring record_path_;
		HWND wake_window_  = nullptr;
		UINT wake_message_ = 0;
	};

} /* namespace wbshterm */
