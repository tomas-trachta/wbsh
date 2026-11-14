/**
 * @file lineedit_highlight.cpp
 * @brief The line editor's syntax colours: the settings, what command words
 *        and paths name, and the PATH listing made off the editing thread.
 */

#include "lineedit.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "highlight.h"
#include "keymap.h"
#include "pathconv.h"

namespace wbsh {

	namespace fs = std::filesystem;

	struct PathListing {
		std::string path;
		std::set<std::string> names;              // as completion offers them
		std::unordered_set<std::string> folded;   // lower case, with and without .exe
		std::chrono::steady_clock::time_point made;
	};

	struct PathListingJob {
		std::string path;
		std::mutex mutex;
		std::condition_variable finished;
		std::shared_ptr<const PathListing> listing;
	};

	namespace listing_detail {

		static const std::size_t kPathCacheLimit = 512;

		// A program installed after PATH was listed is known by the first
		// prompt this long after the listing.
		static const std::chrono::seconds kListingLifetime(60);

		static const char* const kExecutableExtensions[] = { ".exe", ".cmd", ".bat", ".com", nullptr };

		static const char* const kSystemDirs[] = {
			"/bin/", "/sbin/", "/usr/bin/", "/usr/sbin/", "/usr/local/bin/", nullptr,
		};

		static std::string folded(std::string text) {
			for (char& letter : text) {
				letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
			}

			return text;
		}

		static bool endsWithLowered(const std::string& name, const char* suffix) {
			const std::size_t length = std::strlen(suffix);
			if (name.size() <= length) return false;

			return folded(name.substr(name.size() - length)) == suffix;
		}

		static std::string stripExeExtension(std::string name) {
			for (int i = 0; kExecutableExtensions[i] != nullptr; ++i) {
				if (!endsWithLowered(name, kExecutableExtensions[i])) continue;

				name.resize(name.size() - std::strlen(kExecutableExtensions[i]));
				break;
			}

			return name;
		}

		static void addListedName(PathListing& listing, const std::string& name) {
			const std::string stripped = stripExeExtension(name);
			listing.names.insert(stripped);
			listing.folded.insert(folded(name));
			listing.folded.insert(folded(stripped));
		}

		static void listDirectory(PathListing& listing, const fs::path& dir) {
			std::error_code ec;
			fs::directory_iterator entry(dir, ec);
			const fs::directory_iterator end;
			while (!ec && entry != end) {
				addListedName(listing, pathToUtf8(entry->path().filename()));
				entry.increment(ec);
			}
		}

		static void runListing(std::shared_ptr<PathListingJob> job, std::vector<fs::path> dirs) {
			auto listing = std::make_shared<PathListing>();
			listing->path = job->path;
			for (const fs::path& dir : dirs) listDirectory(*listing, dir);
			listing->made = std::chrono::steady_clock::now();

			std::unique_lock<std::mutex> lock(job->mutex);
			job->listing = std::move(listing);
			lock.unlock();

			job->finished.notify_all();
			LineEditor::requestPromptRefresh();
		}

		static std::shared_ptr<const PathListing> takeListing(PathListingJob& job, bool wait) {
			std::unique_lock<std::mutex> lock(job.mutex);
			if (wait) job.finished.wait(lock, [&job] { return job.listing != nullptr; });

			return job.listing;
		}

		static bool expired(const PathListing& listing) {
			return std::chrono::steady_clock::now() - listing.made > kListingLifetime;
		}

		// `/usr/bin/env` runs `env` when no such file exists, as the
		// executor has it.
		static std::string systemBinName(const std::string& word) {
			for (int i = 0; kSystemDirs[i] != nullptr; ++i) {
				const std::size_t length = std::strlen(kSystemDirs[i]);
				if (word.size() <= length || word.compare(0, length, kSystemDirs[i]) != 0) continue;

				const std::string name = word.substr(length);
				return name.find('/') == std::string::npos ? name : std::string();
			}

			return {};
		}

		static bool looksLikePath(const std::string& word) {
			return word.find_first_of("/\\:") != std::string::npos || word[0] == '~';
		}

		static std::string withHome(const std::string& word, const std::string& home) {
			const bool tilde = word == "~" || word.compare(0, 2, "~/") == 0;
			return tilde && !home.empty() ? home + word.substr(1) : word;
		}

		static std::string unquoted(const std::string& value) {
			const bool quoted = value.size() >= 2 && (value[0] == '"' || value[0] == '\'')
				&& value.back() == value[0];
			return quoted ? value.substr(1, value.size() - 2) : value;
		}

#ifdef _WIN32
		static PathKind pathKindOf(const fs::path& path) {
			const DWORD attributes = ::GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES) return PathKind::kMissing;

			const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			return directory ? PathKind::kDirectory : PathKind::kFile;
		}

		static bool driveIsRemote(wchar_t letter) {
			const wchar_t root[] = { letter, L':', L'\\', L'\0' };
			return ::GetDriveTypeW(root) == DRIVE_REMOTE;
		}
#else
		static PathKind pathKindOf(const fs::path& path) {
			std::error_code ec;
			const fs::file_status status = fs::status(path, ec);
			if (ec || !fs::exists(status)) return PathKind::kMissing;

			return fs::is_directory(status) ? PathKind::kDirectory : PathKind::kFile;
		}

		static bool driveIsRemote(wchar_t) {
			return false;
		}
#endif /* _WIN32 */

	}  // namespace listing_detail

	void LineEditor::setHighlightContext(const std::string& pending) {
		highlight_context_ = pending;
	}

	bool LineEditor::highlightWanted() const {
		if (headless_ && !replay_highlight_) return false;
		if (minibuffer_active_ || !shellKeymap().isOn("syntax-highlight")) return false;
		if (!env_.get("NO_COLOR").empty()) return false;

		return env_.get("TERM") != "dumb";
	}

	// The lines this one continues are scanned with it, so a quote or a
	// here-document opened on them colours it, but only it is painted.
	std::string LineEditor::highlightedLine() {
		highlight_waiting_ = false;
		if (!highlightWanted()) return buffer_;

		HighlightLookup lookup;
		lookup.command = [this](const std::string& word) { return highlightCommandRole(word); };
		lookup.existing_path = [this](const std::string& word) { return highlightPathExists(word); };

		const std::string text = highlight_context_ + buffer_;
		const std::vector<HighlightRole> roles = highlightRoles(text, lookup);
		return paintHighlighted(text, roles, highlight_context_.size(), highlightPalette());
	}

	// inputrc's colours first, WBSH_HIGHLIGHT's over them.
	const HighlightPalette& LineEditor::highlightPalette() {
		const std::string inputrc = shellKeymap().variable("syntax-highlight-colors");
		const std::string spec = listing_detail::unquoted(inputrc) + ":" + env_.get("WBSH_HIGHLIGHT");
		if (highlight_palette_ready_ && spec == highlight_spec_) return highlight_palette_;

		highlight_palette_ = defaultHighlightPalette();
		applyHighlightSpec(spec, highlight_palette_);
		highlight_spec_ = spec;
		highlight_palette_ready_ = true;
		return highlight_palette_;
	}

	// `!!` and `!-2` are history expansions, which name a command only
	// once expanded.
	HighlightRole LineEditor::highlightCommandRole(const std::string& word) {
		if (word.empty() || word[0] == '!') return HighlightRole::kPlain;
		if (namesShellCommand(word)) return HighlightRole::kCommand;
		if (listing_detail::looksLikePath(word)) return pathCommandRole(word);

		return listedCommandRole(word);
	}

	bool LineEditor::namesShellCommand(const std::string& word) const {
		return exec_.aliases().count(word) != 0 || exec_.isBuiltin(word) || exec_.isFunction(word)
		    || exec_.pluginCommand(word) != nullptr;
	}

	// Until PATH is listed a word is left uncoloured, and coloured once
	// the listing comes in; a replay waits for the listing instead.
	HighlightRole LineEditor::listedCommandRole(const std::string& word) {
		const std::shared_ptr<const PathListing> listing = pathListing(headless_);
		if (!listing) {
			highlight_waiting_ = true;
			return HighlightRole::kPlain;
		}

		const bool listed = listing->folded.count(listing_detail::folded(word)) != 0;
		return listed ? HighlightRole::kCommand : HighlightRole::kUnknown;
	}

	HighlightRole LineEditor::pathCommandRole(const std::string& word) {
		const PathKind kind = cachedPathKind(word, true);
		if (kind == PathKind::kFile) return HighlightRole::kCommand;
		if (kind == PathKind::kUnreachable) return HighlightRole::kPlain;

		const std::string name = listing_detail::systemBinName(word);
		if (name.empty()) return HighlightRole::kUnknown;

		return namesShellCommand(name) ? HighlightRole::kCommand : listedCommandRole(name);
	}

	bool LineEditor::highlightPathExists(const std::string& word) {
		const PathKind kind = cachedPathKind(word, false);
		return kind == PathKind::kFile || kind == PathKind::kDirectory;
	}

	PathKind LineEditor::cachedPathKind(const std::string& word, bool as_command) {
		const std::string key = (as_command ? "x:" : "p:") + word;
		const auto found = highlight_paths_.find(key);
		if (found != highlight_paths_.end()) return found->second;

		if (highlight_paths_.size() >= listing_detail::kPathCacheLimit) highlight_paths_.clear();

		const PathKind kind = probePath(word, as_command);
		highlight_paths_.emplace(key, kind);
		return kind;
	}

	// A command may leave out its .exe; the console finds it all the same.
	PathKind LineEditor::probePath(const std::string& word, bool as_command) {
		const std::string expanded = listing_detail::withHome(word, env_.get("HOME"));
		const fs::path native = utf8ToPath(exec_.pathConv().toWin32(expanded));
		if (onRemoteDrive(native)) return PathKind::kUnreachable;

		const PathKind kind = listing_detail::pathKindOf(native);
		if (kind != PathKind::kMissing || !as_command) return kind;

		for (int i = 0; listing_detail::kExecutableExtensions[i] != nullptr; ++i) {
			fs::path candidate = native;
			candidate += listing_detail::kExecutableExtensions[i];
			if (listing_detail::pathKindOf(candidate) == PathKind::kFile) return PathKind::kFile;
		}

		return PathKind::kMissing;
	}

	bool LineEditor::onRemoteDrive(const fs::path& path) {
		std::error_code ec;
		const fs::path absolute = path.is_absolute() ? path : fs::current_path(ec);
		const std::wstring root = absolute.root_name().wstring();
		const bool unc = root.size() >= 2 && (root[0] == L'\\' || root[0] == L'/');
		if (unc) return true;
		if (root.size() != 2 || root[1] != L':') return false;

		const wchar_t letter = static_cast<wchar_t>(std::towupper(root[0]));
		if (letter < L'A' || letter > L'Z') return false;

		signed char& remote = drive_remote_[letter - L'A'];
		if (remote == 0) remote = listing_detail::driveIsRemote(letter) ? 1 : -1;
		return remote > 0;
	}

	// A new line probes the disk afresh.
	void LineEditor::resetHighlightCaches() {
		highlight_paths_.clear();
		std::fill(std::begin(drive_remote_), std::end(drive_remote_), static_cast<signed char>(0));
		highlight_waiting_ = false;
	}

	bool LineEditor::highlightSettled() {
		return highlight_waiting_ && pathListing(false) != nullptr;
	}

	void LineEditor::startPathListing(const std::string& path) {
		std::vector<fs::path> dirs;
		for (const std::string& dir : splitPathList(path)) {
			if (!dir.empty()) dirs.push_back(utf8ToPath(exec_.pathConv().toWin32(dir)));
		}

		auto job = std::make_shared<PathListingJob>();
		job->path = path;
		path_listing_job_ = job;
		std::thread(listing_detail::runListing, job, std::move(dirs)).detach();
	}

	// The newest finished listing of this PATH, or null while the first
	// one is still being made. An old listing stands in while a fresh
	// one is made.
	std::shared_ptr<const PathListing> LineEditor::pathListing(bool wait) {
		const std::string path = env_.get("PATH");
		if (!path_listing_job_ || path_listing_job_->path != path) startPathListing(path);

		const bool have = path_listing_ && path_listing_->path == path;
		const std::shared_ptr<const PathListing> latest =
			listing_detail::takeListing(*path_listing_job_, wait && !have);
		if (latest) path_listing_ = latest;
		if (!path_listing_ || path_listing_->path != path) return nullptr;

		if (latest && listing_detail::expired(*latest)) startPathListing(path);
		return path_listing_;
	}

	const std::set<std::string>& LineEditor::pathCommandNames() {
		return pathListing(true)->names;
	}

}  // namespace wbsh
