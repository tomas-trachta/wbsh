/**
 * @file pathconv_args.cpp
 * @brief MSYS2-style translation of the arguments handed to a native program.
 *
 * The rules mirror msys2-runtime's msys2_path_conv.cc, which is what Git Bash
 * applies to the argv of every non-MSYS program. wbsh has no MSYS root, so a
 * POSIX path that no mount covers (`/usr/bin`, `/`) is passed on unchanged.
 */

#include "pathconv.h"

#include <cctype>
#include <string>
#include <vector>

namespace wbsh {

	static const char kNullDevice[] = "nul";
	static const char kWin32ListSeparator = ';';
	static const char kPosixListSeparator = ':';
	static const char kExclusionSeparator = ';';

	// MSYS leaves an argument alone when it holds glob or quote characters
	// (the callee matches or parses those itself), `;` (already a Win32
	// list), `/~`, a C++ `::` or `@@`.
	static const char kStopperCharacters[] = "`'\"*?[];";
	static const char* const kStopperSequences[] = { "/~", "::", "@@" };

	// Characters a path can start with; MSYS skips anything else in front
	// of one, so `@/c/x` and `+/c/x` convert past their first character.
	static const char kPathStartPunctuation[] = "/\\:-.~";

	enum class ColonKind { List, Url, Other };

	static std::string convertFrom(const PathConv& conv, const std::string& text);

	static bool isAsciiAlpha(char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}

	static bool isWin32Switch(const std::string& arg) {
		return arg.size() == 2 && arg[0] == '/' && arg[1] != '/';
	}

	static bool hasConversionStopper(const std::string& arg) {
		if (arg.find_first_of(kStopperCharacters) != std::string::npos) return true;

		for (const char* sequence : kStopperSequences) {
			if (arg.find(sequence) != std::string::npos) return true;
		}

		return false;
	}

	static bool canStartPath(char c) {
		const unsigned char byte = static_cast<unsigned char>(c);
		if (std::isalnum(byte) || byte >= 0x80) return true;

		return std::string(kPathStartPunctuation).find(c) != std::string::npos;
	}

	static std::size_t pathStart(const std::string& text) {
		std::size_t start = 0;
		while (start < text.size() && !canStartPath(text[start])) ++start;
		return start;
	}

	static bool startsWithDrive(const std::string& text) {
		return text.size() >= 2 && isAsciiAlpha(text[0]) && text[1] == ':';
	}

	static bool isDotRelative(const std::string& text) {
		return text[0] == '.' && (text[1] == '.' || text[1] == '/')
			&& text.find(':', 2) == std::string::npos;
	}

	// Mapping these to CONIN$ / CONOUT$ / CON breaks cross-process uses such
	// as `docker exec ... -i /dev/stdin`, where the callee reads the path.
	static bool isPassThroughDevice(const std::string& path) {
		return path == "/dev/stdin" || path == "/dev/stdout"
			|| path == "/dev/stderr" || path == "/dev/tty";
	}

	// MSYS takes a backslash inside a POSIX path for a separator and folds
	// runs of separators into one.
	static std::string normalisedPosixPath(const std::string& path) {
		std::string out;
		for (char c : path) {
			const char unified = c == '\\' ? '/' : c;
			if (unified == '/' && !out.empty() && out.back() == '/') continue;

			out.push_back(unified);
		}

		return out;
	}

	static std::string convertPosixPath(const PathConv& conv, const std::string& path) {
		if (path == "/dev/null") return kNullDevice;
		if (isPassThroughDevice(path)) return path;

		std::string native;
		if (!conv.applyMount(normalisedPosixPath(path), native)) return path;

		return native;
	}

	static std::vector<std::string> splitOnColons(const std::string& list) {
		std::vector<std::string> entries;
		std::size_t start = 0;
		while (true) {
			const std::size_t colon = list.find(kPosixListSeparator, start);
			entries.push_back(list.substr(start, colon - start));
			if (colon == std::string::npos) return entries;

			start = colon + 1;
		}
	}

	static bool convertListEntry(const PathConv& conv, const std::string& entry,
	                             std::string& native) {
		if (entry == "." || entry == "..") {
			native = entry;
			return true;
		}

		if (entry.empty() || entry[0] != '/') return false;

		native = convertPosixPath(conv, entry);
		return native != entry;
	}

	// A list converts only as a whole: with one entry that has no Win32 form
	// (an unmounted `/usr`, the `/bar` of `docker -v /c/foo:/bar`) the
	// argument is left as it was.
	static std::string convertPathList(const PathConv& conv, const std::string& list) {
		std::string out;
		bool first = true;
		for (const std::string& entry : splitOnColons(list)) {
			std::string native;
			if (!convertListEntry(conv, entry, native)) return list;

			if (!first) out.push_back(kWin32ListSeparator);
			out += native;
			first = false;
		}

		return out;
	}

	static ColonKind colonKind(const std::string& text, std::size_t colon) {
		const char next = colon + 1 < text.size() ? text[colon + 1] : '\0';
		const char after = colon + 2 < text.size() ? text[colon + 2] : '\0';
		if (next == '/' && after == '/') return ColonKind::Url;
		if (next == '/' || next == ':' || next == '.') return ColonKind::List;

		return ColonKind::Other;
	}

	// `//x` is MSYS's escape for a Win32 switch (`cmd //c`, `taskkill //F`)
	// and loses one slash; with a further slash it is a UNC path
	// (`//server/share`) and stays.
	static std::string unescapeDoubleSlash(const std::string& text) {
		if (text.size() == 2 || text[2] == '/') return text.substr(1);
		if (text[2] == ':') return text;
		if (text.find('/', 2) != std::string::npos) return text;

		return text.substr(1);
	}

	static std::string convertRooted(const PathConv& conv, const std::string& text) {
		if (text[1] == '/') return unescapeDoubleSlash(text);
		if (startsWithDrive(text.substr(1))) return text.substr(1);
		if (text.compare(1, 2, "..") == 0) return text;

		const std::size_t colon = text.find(':');
		if (colon == std::string::npos) return convertPosixPath(conv, text);
		if (colonKind(text, colon) == ColonKind::List) return convertPathList(conv, text);

		return text;
	}

	static std::size_t optionPrefixLength(const std::string& text) {
		if (text[0] != '-') return 0;
		if (isAsciiAlpha(text[1])) return 2;

		return 1;
	}

	// `-I/c/x`, `-L/c/x`: a path glued to a one-letter option.
	static bool hasGluedPath(const std::string& text, std::size_t prefix) {
		return prefix == 2 && prefix < text.size() && text[prefix] == '/'
			&& text.find(',', prefix) == std::string::npos;
	}

	static std::string convertAfter(const PathConv& conv, const std::string& text,
	                                std::size_t split) {
		return text.substr(0, split) + convertFrom(conv, text.substr(split));
	}

	// A path inside a word: after `=` (`--prefix=/c/x`, `VAR=/c/x`), after
	// `,` in an option (`-Wl,-rpath,/c/x`), or a `:` list of them.
	static std::string convertEmbedded(const PathConv& conv, const std::string& text) {
		const std::size_t prefix = optionPrefixLength(text);
		if (hasGluedPath(text, prefix)) return convertAfter(conv, text, prefix);

		const bool option = text[0] == '-';
		for (std::size_t i = prefix; i < text.size(); ++i) {
			const char c = text[i];
			if (c == '=' || (c == ',' && option)) return convertAfter(conv, text, i + 1);
			if (c != ':') continue;

			const ColonKind kind = colonKind(text, i);
			if (kind == ColonKind::List) return convertPathList(conv, text);
			if (kind == ColonKind::Url) return text;
			if (text.find('=', i) == std::string::npos) return text;
		}

		return text;
	}

	static std::string convertCandidate(const PathConv& conv, const std::string& text) {
		if (text.size() == 1 || text[0] == '~') return text;
		if (startsWithDrive(text) || isDotRelative(text)) return text;
		if (text[0] == '/') return convertRooted(conv, text);

		return convertEmbedded(conv, text);
	}

	static std::string convertFrom(const PathConv& conv, const std::string& text) {
		const std::size_t start = pathStart(text);
		if (start == text.size()) return text;

		return text.substr(0, start) + convertCandidate(conv, text.substr(start));
	}

	// A two-character `/x` stays as it is, unlike in Git Bash, so the Win32
	// switches of `cmd /c`, `where /q` keep working without the `//` escape.
	std::string PathConv::translateArg(const std::string& arg) const {
		if (isWin32Switch(arg) || hasConversionStopper(arg)) return arg;

		return convertFrom(*this, arg);
	}

	bool argExcludedFromConversion(const std::string& arg, const std::string& exclusions) {
		if (exclusions == "*") return true;

		std::size_t start = 0;
		while (start <= exclusions.size()) {
			std::size_t end = exclusions.find(kExclusionSeparator, start);
			if (end == std::string::npos) end = exclusions.size();

			const std::size_t length = end - start;
			if (length > 0 && arg.compare(0, length, exclusions, start, length) == 0) return true;

			start = end + 1;
		}

		return false;
	}

}  // namespace wbsh
