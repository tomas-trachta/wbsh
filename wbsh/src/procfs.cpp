/**
 * @file procfs.cpp
 * @brief Resolving /proc paths, listing its directories, and opening its files.
 */

#include "procfs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cerrno>
#include <string>
#include <thread>
#include <vector>

#include "numparse.h"
#include "procfs_internal.h"

namespace wbsh {

	namespace procfs_detail {

		using Parts = std::vector<std::string>;

		/// One path below /proc, its links not followed.
		struct Entry {
			ProcType type = ProcType::Missing;
			std::string target;
			/// A link inside /proc: the parts it stands for.
			Parts into;
			/// A link out of /proc: the POSIX path.
			std::string outside;
			int fd = -1;
		};

		typedef std::string (*TextMaker)();

		struct SystemFile {
			const char* name;
			TextMaker make;
		};

		static const SystemFile kSystemFiles[] = {
			{ "cpuinfo", cpuInfoText },
			{ "loadavg", loadAverageText },
			{ "meminfo", memInfoText },
			{ "uptime",  uptimeFileText },
			{ "version", versionText },
		};

		static const char* const kProcessFiles[] = { "cmdline", "comm", "status" };
		static const char* const kProcessEntries[] = { "cmdline", "comm", "exe", "status" };
		static const char* const kSelfEntries[] = { "cwd", "environ", "fd" };

	}  // namespace procfs_detail

	using procfs_detail::Entry;
	using procfs_detail::Parts;

	static const char kSelf[] = "self";

	static bool foldPath(const std::string& posix_path, Parts& parts) {
		if (posix_path.empty() || posix_path[0] != '/') return false;

		std::size_t at = 0;
		while (at < posix_path.size()) {
			const std::size_t slash = posix_path.find('/', at);
			const std::size_t end = slash == std::string::npos ? posix_path.size() : slash;
			const std::string part = posix_path.substr(at, end - at);
			at = end + 1;
			if (part.empty() || part == ".") continue;

			if (part != "..") parts.push_back(part);
			else if (!parts.empty()) parts.pop_back();
		}

		return true;
	}

	static bool belowProc(const std::string& posix_path, Parts& below) {
		Parts parts;
		if (!foldPath(posix_path, parts) || parts.empty() || parts[0] != "proc") return false;

		below.assign(parts.begin() + 1, parts.end());
		return true;
	}

	bool isProcPath(const std::string& posix_path) {
		Parts below;
		return belowProc(posix_path, below);
	}

	static std::string joinProcPath(const Parts& parts) {
		std::string path = "/proc";
		for (const std::string& part : parts) path += "/" + part;
		return path;
	}

	static bool parsePid(const std::string& text, unsigned long& pid) {
		unsigned long long value = 0;
		std::size_t used = 0;
		if (text.empty() || text[0] == '0' || !parseULL(text, value, &used, 10)) return false;
		if (used != text.size() || value > 0xFFFFFFFFull) return false;

		pid = static_cast<unsigned long>(value);
		return true;
	}

	static bool isSelf(unsigned long pid) {
		return pid == ::GetCurrentProcessId();
	}

	template <std::size_t N>
	static bool listed(const char* const (&names)[N], const std::string& name) {
		for (const char* candidate : names) {
			if (name == candidate) return true;
		}

		return false;
	}

	static procfs_detail::TextMaker systemFileMaker(const std::string& name) {
		for (const procfs_detail::SystemFile& file : procfs_detail::kSystemFiles) {
			if (name == file.name) return file.make;
		}

		return nullptr;
	}

	static Entry typed(ProcType type) {
		Entry entry;
		entry.type = type;
		return entry;
	}

	static Entry linkOutside(const std::string& path) {
		if (path.empty()) return Entry();

		Entry entry = typed(ProcType::Link);
		entry.target = path;
		entry.outside = path;
		return entry;
	}

	static Entry descriptorEntry(const PathConv& paths, const std::string& name) {
		unsigned long fd = 0;
		std::size_t used = 0;
		if (name.empty() || !parseUL(name, fd, 10, &used) || used != name.size()) return Entry();

		std::string disk_path;
		const int descriptor = static_cast<int>(fd);
		const std::string target = procfs_detail::descriptorTarget(paths, descriptor, disk_path);
		if (target.empty()) return Entry();

		Entry entry = disk_path.empty() ? typed(ProcType::Link) : linkOutside(disk_path);
		entry.target = target;
		entry.fd = static_cast<int>(fd);
		return entry;
	}

	static Entry selfEntry(const PathConv& paths, const std::string& name) {
		if (name == "cwd") return linkOutside(procfs_detail::currentDirectoryPath(paths));
		if (name == "environ") return typed(ProcType::File);
		if (name == "fd") return typed(ProcType::Directory);
		return Entry();
	}

	static Entry processEntry(const PathConv& paths, unsigned long pid, const Parts& parts) {
		if (parts.size() == 1) return typed(ProcType::Directory);

		const std::string& name = parts[1];
		const bool own_descriptor = parts.size() == 3 && name == "fd" && isSelf(pid);
		if (own_descriptor) return descriptorEntry(paths, parts[2]);
		if (parts.size() != 2) return Entry();

		if (listed(procfs_detail::kProcessFiles, name)) return typed(ProcType::File);
		if (name == "exe") return linkOutside(procfs_detail::processImagePath(paths, pid));
		return isSelf(pid) ? selfEntry(paths, name) : Entry();
	}

	static Entry selfLink() {
		const std::string pid = std::to_string(::GetCurrentProcessId());
		Entry entry = typed(ProcType::Link);
		entry.target = pid;
		entry.into = { pid };
		return entry;
	}

	static Entry entryAt(const PathConv& paths, const Parts& parts) {
		if (parts.empty()) return typed(ProcType::Directory);

		if (parts[0] == kSelf) return parts.size() == 1 ? selfLink() : Entry();
		if (systemFileMaker(parts[0]) != nullptr) {
			return parts.size() == 1 ? typed(ProcType::File) : Entry();
		}

		unsigned long pid = 0;
		if (!parsePid(parts[0], pid) || !procfs_detail::processExists(pid)) return Entry();
		return processEntry(paths, pid, parts);
	}

	static std::string withRest(const std::string& path, const Parts& rest, std::size_t from) {
		std::string joined = path;
		for (std::size_t i = from; i < rest.size(); ++i) {
			if (joined.empty() || joined.back() != '/') joined.push_back('/');
			joined += rest[i];
		}

		return joined;
	}

	static ProcNode nodeOf(const Entry& entry, const Parts& done) {
		ProcNode node;
		node.type = entry.type;
		node.canonical = joinProcPath(done);
		node.target = entry.target;
		node.fd = entry.fd;
		return node;
	}

	// Following a link either moves within /proc, leaves it (the rest of
	// the path then lies outside), or, for a pipe or console descriptor,
	// ends at a file read through that descriptor.
	static bool followLink(const Entry& entry, const Parts& rest, std::size_t next,
	                       Parts& done, ProcNode& node) {
		if (!entry.into.empty()) {
			done = entry.into;
			return false;
		}

		if (!entry.outside.empty()) {
			node.type = ProcType::Outside;
			node.target = withRest(entry.outside, rest, next);
			return true;
		}

		node = nodeOf(entry, done);
		node.type = next == rest.size() ? ProcType::File : ProcType::Missing;
		return true;
	}

	ProcNode lookupProcPath(const PathConv& paths, const std::string& posix_path,
	                        bool follow_last) {
		Parts rest;
		if (!belowProc(posix_path, rest)) return ProcNode();

		Parts done;
		Entry entry = entryAt(paths, done);
		for (std::size_t i = 0; i < rest.size(); ++i) {
			if (entry.type != ProcType::Directory) return ProcNode();

			done.push_back(rest[i]);
			entry = entryAt(paths, done);
			const bool last = i + 1 == rest.size();
			if (entry.type != ProcType::Link || (last && !follow_last)) continue;

			ProcNode node;
			if (followLink(entry, rest, i + 1, done, node)) return node;
			entry = entryAt(paths, done);
		}

		return nodeOf(entry, done);
	}

	static std::vector<std::string> rootNames() {
		std::vector<std::string> names;
		for (const unsigned long pid : procfs_detail::processIds()) {
			names.push_back(std::to_string(pid));
		}

		names.push_back(kSelf);
		for (const procfs_detail::SystemFile& file : procfs_detail::kSystemFiles) {
			names.push_back(file.name);
		}

		return names;
	}

	static std::vector<std::string> processNames(unsigned long pid) {
		std::vector<std::string> names(std::begin(procfs_detail::kProcessEntries),
			std::end(procfs_detail::kProcessEntries));
		if (isSelf(pid)) {
			names.insert(names.end(), std::begin(procfs_detail::kSelfEntries),
				std::end(procfs_detail::kSelfEntries));
		}

		return names;
	}

	static std::vector<std::string> descriptorNames() {
		std::vector<std::string> names;
		for (const int fd : procfs_detail::openDescriptors()) names.push_back(std::to_string(fd));
		return names;
	}

	std::vector<std::string> listProcDirectory(const PathConv& paths,
	                                           const std::string& posix_path) {
		const ProcNode node = lookupProcPath(paths, posix_path, true);
		if (node.type != ProcType::Directory) return {};

		Parts parts;
		belowProc(node.canonical, parts);
		if (parts.empty()) return rootNames();

		unsigned long pid = 0;
		parsePid(parts[0], pid);
		return parts.size() == 1 ? processNames(pid) : descriptorNames();
	}

	static std::string fileText(const ProcNode& node) {
		Parts parts;
		belowProc(node.canonical, parts);
		if (parts.size() == 1) return systemFileMaker(parts[0])();

		unsigned long pid = 0;
		parsePid(parts[0], pid);
		return procfs_detail::processFileText(pid, parts[1]);
	}

	// Runs until the text is written or the reader has gone.
	static void feedText(HANDLE write_end, std::string text) {
		std::size_t sent = 0;
		while (sent < text.size()) {
			DWORD written = 0;
			const DWORD chunk = static_cast<DWORD>(text.size() - sent);
			if (::WriteFile(write_end, text.data() + sent, chunk, &written, nullptr) == 0) break;
			sent += written;
		}

		::CloseHandle(write_end);
	}

	static int pipeReading(std::string text) {
		HANDLE read_end = nullptr;
		HANDLE write_end = nullptr;
		if (::CreatePipe(&read_end, &write_end, nullptr, 0) == 0) {
			errno = EMFILE;
			return -1;
		}

		const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(read_end), _O_RDONLY | _O_BINARY);
		if (fd < 0) {
			::CloseHandle(read_end);
			::CloseHandle(write_end);
			return -1;
		}

		std::thread(feedText, write_end, std::move(text)).detach();
		return fd;
	}

	static int failWith(int error) {
		errno = error;
		return -1;
	}

	int openProcNode(const PathConv&, const ProcNode& node, int flags) {
		if (node.type == ProcType::Directory) return failWith(EISDIR);
		if (node.type != ProcType::File) return failWith(ENOENT);
		if (node.fd >= 0) return _dup(node.fd);
		if ((flags & (_O_WRONLY | _O_RDWR)) != 0) return failWith(EACCES);

		return pipeReading(fileText(node));
	}

	std::FILE* fopenProcNode(const PathConv& paths, const ProcNode& node, const char* mode) {
		const bool reading = mode[0] == 'r' && std::string(mode).find('+') == std::string::npos;
		const int fd = openProcNode(paths, node, reading ? _O_RDONLY : _O_WRONLY);
		if (fd < 0) return nullptr;

		std::FILE* stream = _fdopen(fd, reading ? "rb" : "wb");
		if (stream == nullptr) _close(fd);
		return stream;
	}

}  // namespace wbsh
