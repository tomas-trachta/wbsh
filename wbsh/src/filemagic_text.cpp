/**
 * @file filemagic_text.cpp
 * @brief Text for `file`: the encoding (ASCII, UTF-8 with or without BOM,
 *        UTF-16, ISO-8859, other 8-bit), line terminators and long lines,
 *        and what the text is: scripts by their #! line, HTML, XML, SVG,
 *        JSON, PEM, C / C++, batch files, PowerShell and makefiles.
 *
 * The byte classes are file(1)'s: text is printable ASCII plus BEL, BS,
 * HT, LF, VT, FF, CR and ESC; ISO-8859 adds 0xA0-0xFF, and "extended
 * ASCII" also 0x80-0x9F.
 */

#include "filemagic.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "utf8.h"

namespace wbsh {

	namespace text_detail {

		enum class Encoding { Ascii, Utf8, Utf8Bom, Utf16Le, Utf16Be, Iso8859, Extended };

		enum class Expect { Value, ValueOrEnd, Key, KeyOrEnd, Colon, CommaOrEnd, Done };

		struct TextFacts {
			std::size_t crlf = 0;
			std::size_t cr = 0;
			std::size_t lf = 0;
			std::size_t longest = 0;
			bool escapes = false;
			bool overstriking = false;
		};

		struct TextKind {
			std::string label;
			const char* mime = "text/plain";
			bool executable = false;
			bool whole = false;
			bool bare = false;
		};

		struct Interpreter {
			const char* name;
			const char* label;
			const char* mime;
		};

		struct PemLabel {
			const char* armor;
			const char* label;
		};

	}  // namespace text_detail

	using text_detail::Encoding;
	using text_detail::Expect;
	using text_detail::Interpreter;
	using text_detail::PemLabel;
	using text_detail::TextFacts;
	using text_detail::TextKind;

	static const Interpreter kInterpreters[] = {
		{ "sh", "POSIX shell script", "text/x-shellscript" },
		{ "bash", "Bourne-Again shell script", "text/x-shellscript" },
		{ "wbsh", "Bourne-Again shell script", "text/x-shellscript" },
		{ "dash", "POSIX shell script", "text/x-shellscript" },
		{ "zsh", "Paul Falstad's zsh script", "text/x-shellscript" },
		{ "ksh", "Korn shell script", "text/x-shellscript" },
		{ "csh", "C shell script", "text/x-shellscript" },
		{ "tcsh", "Tenex C shell script", "text/x-shellscript" },
		{ "python", "Python script", "text/x-script.python" },
		{ "perl", "Perl script", "text/x-perl" },
		{ "ruby", "Ruby script", "text/x-ruby" },
		{ "node", "Node.js script", "application/javascript" },
		{ "nodejs", "Node.js script", "application/javascript" },
		{ "php", "PHP script", "text/x-php" },
		{ "lua", "Lua script", "text/x-lua" },
		{ "awk", "awk or perl script", "text/x-awk" },
		{ "gawk", "GNU awk script", "text/x-awk" },
		{ "tclsh", "Tcl script", "text/x-tcl" },
		{ "wish", "Tcl/Tk script", "text/x-tcl" },
		{ "pwsh", "PowerShell script", "text/plain" },
		{ "make", "makefile script", "text/x-makefile" },
	};

	static const PemLabel kPemLabels[] = {
		{ "CERTIFICATE-----", "PEM certificate" },
		{ "CERTIFICATE REQUEST-----", "PEM certificate request" },
		{ "NEW CERTIFICATE REQUEST-----", "PEM certificate request" },
		{ "RSA PRIVATE KEY-----", "PEM RSA private key" },
		{ "DSA PRIVATE KEY-----", "PEM DSA private key" },
		{ "EC PRIVATE KEY-----", "PEM EC private key" },
		{ "OPENSSH PRIVATE KEY-----", "OpenSSH private key" },
		{ "PGP PUBLIC KEY BLOCK-----", "PGP public key block" },
		{ "PGP PRIVATE KEY BLOCK-----", "PGP private key block" },
		{ "PGP MESSAGE-----", "PGP message" },
		{ "PGP SIGNATURE-----", "PGP signature" },
	};

	static const std::size_t kLongLine = 300;
	static const std::size_t kHeadSearch = 4096;
	static const std::uint8_t kEscape = 0x1b;
	static const std::uint8_t kBackspace = 0x08;

	static bool isTextByte(std::uint8_t c) {
		return (c >= 0x20 && c < 0x7f) || (c >= 7 && c <= 13) || c == kEscape;
	}

	static bool isTextCode(char32_t c) {
		return c >= 0x80 || isTextByte(static_cast<std::uint8_t>(c));
	}

	static bool isInvalidUtf8(const Utf8Char& decoded) {
		return decoded.length == 1 && decoded.code >= 0xdc80 && decoded.code <= 0xdcff;
	}

	static bool decodeUtf8Text(const std::string& bytes, bool truncated, std::u32string& codes,
	                           bool& multibyte) {
		multibyte = false;
		for (std::size_t at = 0; at < bytes.size();) {
			const Utf8Char decoded = decodeUtf8(bytes, at);
			if (isInvalidUtf8(decoded)) return truncated && bytes.size() - at < 4;
			if (!isTextCode(decoded.code)) return false;

			multibyte = multibyte || decoded.length > 1;
			codes.push_back(decoded.code);
			at += decoded.length;
		}

		return true;
	}

	static char32_t utf16Unit(const std::string& bytes, std::size_t at, bool little) {
		const std::uint32_t first = static_cast<std::uint8_t>(bytes[at]);
		const std::uint32_t second = static_cast<std::uint8_t>(bytes[at + 1]);
		return static_cast<char32_t>(little ? first | (second << 8) : (first << 8) | second);
	}

	static bool decodeUtf16Text(const std::string& bytes, bool little, std::u32string& codes) {
		for (std::size_t at = 2; at + 1 < bytes.size(); at += 2) {
			char32_t code = utf16Unit(bytes, at, little);
			const bool high = code >= 0xd800 && code < 0xdc00;
			if (high && at + 3 < bytes.size()) {
				const char32_t low = utf16Unit(bytes, at + 2, little);
				code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
				at += 2;
			}

			if (code == 0 || !isTextCode(code)) return false;
			codes.push_back(code);
		}

		return true;
	}

	static bool decodeEightBit(const std::string& bytes, std::u32string& codes,
	                           Encoding& encoding) {
		encoding = Encoding::Ascii;
		for (const char byte : bytes) {
			const std::uint8_t c = static_cast<std::uint8_t>(byte);
			if (c < 0x80 && !isTextByte(c)) return false;
			if (c >= 0xa0 && encoding == Encoding::Ascii) encoding = Encoding::Iso8859;
			if (c >= 0x80 && c < 0xa0) encoding = Encoding::Extended;
			codes.push_back(c);
		}

		return true;
	}

	static bool startsWithBytes(const std::string& bytes, const char* prefix, std::size_t count) {
		return bytes.size() >= count && bytes.compare(0, count, prefix, count) == 0;
	}

	static bool decodeText(const FileSample& sample, Encoding& encoding, std::u32string& codes) {
		const std::string bytes(sample.bytes.begin(), sample.bytes.end());
		const bool truncated = sample.size > bytes.size();
		if (startsWithBytes(bytes, "\xff\xfe", 2) || startsWithBytes(bytes, "\xfe\xff", 2)) {
			const bool little = bytes[0] == '\xff';
			encoding = little ? Encoding::Utf16Le : Encoding::Utf16Be;
			return decodeUtf16Text(bytes, little, codes);
		}

		bool multibyte = false;
		const bool bom = startsWithBytes(bytes, "\xef\xbb\xbf", 3);
		std::u32string utf8;
		if (decodeUtf8Text(bom ? bytes.substr(3) : bytes, truncated, utf8, multibyte)) {
			codes = utf8;
			encoding = bom ? Encoding::Utf8Bom : multibyte ? Encoding::Utf8 : Encoding::Ascii;
			return true;
		}

		return decodeEightBit(bytes, codes, encoding);
	}

	static void countTerminator(const std::u32string& codes, std::size_t at, TextFacts& facts) {
		if (codes[at] == '\n') {
			++facts.lf;
			return;
		}

		if (at + 1 < codes.size() && codes[at + 1] == '\n') ++facts.crlf;
		else ++facts.cr;
	}

	static TextFacts gatherFacts(const std::u32string& codes) {
		TextFacts facts;
		std::size_t line = 0;
		for (std::size_t at = 0; at < codes.size(); ++at) {
			const char32_t c = codes[at];
			if (c == kEscape) facts.escapes = true;
			if (c == kBackspace) facts.overstriking = true;
			if (c != '\n' && c != '\r') {
				++line;
				continue;
			}

			facts.longest = std::max(facts.longest, line);
			line = 0;
			countTerminator(codes, at, facts);
			if (c == '\r' && at + 1 < codes.size() && codes[at + 1] == '\n') ++at;
		}

		facts.longest = std::max(facts.longest, line);
		return facts;
	}

	static std::string terminatorText(const TextFacts& facts) {
		if (facts.crlf == 0 && facts.cr == 0 && facts.lf == 0) return ", with no line terminators";
		if (facts.crlf == 0 && facts.cr == 0) return std::string();

		std::string kinds;
		if (facts.crlf != 0) kinds += "CRLF";
		if (facts.cr != 0) kinds += kinds.empty() ? "CR" : ", CR";
		if (facts.lf != 0) kinds += ", LF";
		return ", with " + kinds + " line terminators";
	}

	static std::string factsText(const TextFacts& facts) {
		std::string text;
		if (facts.longest > kLongLine) {
			text += ", with very long lines (" + std::to_string(facts.longest) + ")";
		}

		text += terminatorText(facts);
		if (facts.escapes) text += ", with escape sequences";
		if (facts.overstriking) text += ", with overstriking";
		return text;
	}

	static const char* encodingLabel(Encoding encoding) {
		switch (encoding) {
			case Encoding::Ascii:
				return "ASCII";
			case Encoding::Utf8:
				return "Unicode text, UTF-8";
			case Encoding::Utf8Bom:
				return "Unicode text, UTF-8 (with BOM)";
			case Encoding::Utf16Le:
				return "Unicode text, UTF-16, little-endian";
			case Encoding::Utf16Be:
				return "Unicode text, UTF-16, big-endian";
			case Encoding::Iso8859:
				return "ISO-8859";
			default:
				return "Non-ISO extended-ASCII";
		}
	}

	static const char* charsetName(Encoding encoding) {
		switch (encoding) {
			case Encoding::Ascii:
				return "us-ascii";
			case Encoding::Utf8:
			case Encoding::Utf8Bom:
				return "utf-8";
			case Encoding::Utf16Le:
				return "utf-16le";
			case Encoding::Utf16Be:
				return "utf-16be";
			case Encoding::Iso8859:
				return "iso-8859-1";
			default:
				return "unknown-8bit";
		}
	}

	static std::string asUtf8(const std::u32string& codes) {
		std::string text;
		for (const char32_t code : codes) appendUtf8(text, code);
		return text;
	}

	static std::string lowered(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static std::size_t skipSpace(const std::string& text, std::size_t at) {
		while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) ++at;
		return at;
	}

	static bool startsAt(const std::string& text, std::size_t at, const char* prefix) {
		return text.compare(at, std::char_traits<char>::length(prefix), prefix) == 0;
	}

	static std::vector<std::string> shebangWords(const std::string& text) {
		const std::size_t end = std::min(text.find('\n'), text.size());
		std::string line = text.substr(2, end - 2);
		if (!line.empty() && line.back() == '\r') line.pop_back();

		std::vector<std::string> words;
		std::size_t at = 0;
		while ((at = line.find_first_not_of(" \t", at)) != std::string::npos) {
			const std::size_t stop = std::min(line.find_first_of(" \t", at), line.size());
			words.push_back(line.substr(at, stop - at));
			at = stop;
		}

		return words;
	}

	static std::string programName(const std::string& path) {
		std::string name = path.substr(path.find_last_of("/\\") + 1);
		const bool exe = name.size() > 4 && lowered(name.substr(name.size() - 4)) == ".exe";
		if (exe) name.resize(name.size() - 4);
		while (!name.empty() && (std::isdigit(static_cast<unsigned char>(name.back())) != 0
				|| name.back() == '.')) {
			name.pop_back();
		}

		return name;
	}

	static std::string interpreterOf(const std::vector<std::string>& words) {
		if (programName(words[0]) != "env") return programName(words[0]);

		for (std::size_t k = 1; k < words.size(); ++k) {
			if (words[k][0] != '-' && words[k].find('=') == std::string::npos) {
				return programName(words[k]);
			}
		}

		return std::string();
	}

	static std::string joinedWords(const std::vector<std::string>& words) {
		std::string text;
		for (const std::string& word : words) text += (text.empty() ? "" : " ") + word;
		return text;
	}

	static bool detectShebang(const std::string& text, TextKind& kind) {
		if (!startsAt(text, 0, "#!")) return false;

		const std::vector<std::string> words = shebangWords(text);
		if (words.empty()) return false;

		kind.executable = true;
		const std::string interpreter = interpreterOf(words);
		for (const Interpreter& known : kInterpreters) {
			if (interpreter != known.name) continue;
			kind.label = known.label;
			kind.mime = known.mime;
			kind.bare = interpreter == "perl";
			return true;
		}

		kind.label = "a " + joinedWords(words) + " script";
		return true;
	}

	static bool detectPem(const std::string& text, TextKind& kind) {
		const std::size_t at = skipSpace(text, 0);
		if (!startsAt(text, at, "-----BEGIN ")) return false;

		for (const PemLabel& pem : kPemLabels) {
			if (!startsAt(text, at + 11, pem.armor)) continue;
			kind.label = pem.label;
			kind.whole = true;
			return true;
		}

		return false;
	}

	static bool detectMarkup(const std::string& text, TextKind& kind) {
		const std::size_t at = skipSpace(text, 0);
		const std::string head = lowered(text.substr(at, kHeadSearch));
		const bool xml = startsAt(head, 0, "<?xml");
		if (head.find("<svg") != std::string::npos && (xml || startsAt(head, 0, "<svg"))) {
			kind.label = "SVG Scalable Vector Graphics image";
			kind.mime = "image/svg+xml";
			kind.whole = true;
			return true;
		}

		if (xml) {
			const std::size_t version = head.find("version=");
			const std::string number = version == std::string::npos ? "1.0"
				: head.substr(version + 9, 3);
			kind.label = "XML " + number + " document";
			kind.mime = "text/xml";
			return true;
		}

		static const char* const kHtmlStarts[] = {
			"<!doctype html", "<html", "<head", "<body", "<title", "<script",
		};
		for (const char* start : kHtmlStarts) {
			if (!startsAt(head, 0, start)) continue;
			kind.label = "HTML document";
			kind.mime = "text/html";
			return true;
		}

		return false;
	}

	static bool scanJsonString(const std::string& text, std::size_t& at) {
		if (at >= text.size() || text[at] != '"') return false;

		for (++at; at < text.size(); ++at) {
			const unsigned char c = static_cast<unsigned char>(text[at]);
			if (c < 0x20) return false;
			if (c == '"') {
				++at;
				return true;
			}

			if (c == '\\') ++at;
		}

		return false;
	}

	static bool scanJsonNumber(const std::string& text, std::size_t& at) {
		const std::size_t start = at;
		if (at < text.size() && text[at] == '-') ++at;
		const std::size_t digits = at;
		while (at < text.size() && (std::isdigit(static_cast<unsigned char>(text[at])) != 0
				|| std::strchr(".eE+-", text[at]) != nullptr)) {
			++at;
		}

		return at > digits && at > start;
	}

	static bool scanJsonScalar(const std::string& text, std::size_t& at) {
		static const char* const kWords[] = { "true", "false", "null" };
		if (text[at] == '"') return scanJsonString(text, at);
		for (const char* word : kWords) {
			if (!startsAt(text, at, word)) continue;
			at += std::char_traits<char>::length(word);
			return true;
		}

		return scanJsonNumber(text, at);
	}

	static Expect afterValue(const std::vector<char>& stack) {
		return stack.empty() ? Expect::Done : Expect::CommaOrEnd;
	}

	static bool closeContainer(const std::string& text, std::size_t& at, std::vector<char>& stack,
	                           Expect& expect) {
		const char wanted = stack.back() == '{' ? '}' : ']';
		if (text[at] != wanted) return false;

		stack.pop_back();
		++at;
		expect = afterValue(stack);
		return true;
	}

	static bool jsonValueStep(const std::string& text, std::size_t& at, std::vector<char>& stack,
	                          Expect& expect) {
		const char c = text[at];
		if (expect == Expect::ValueOrEnd && c == ']') {
			return closeContainer(text, at, stack, expect);
		}

		if (c == '{' || c == '[') {
			stack.push_back(c);
			++at;
			expect = c == '{' ? Expect::KeyOrEnd : Expect::ValueOrEnd;
			return true;
		}

		if (!scanJsonScalar(text, at)) return false;
		expect = afterValue(stack);
		return true;
	}

	static bool jsonStep(const std::string& text, std::size_t& at, std::vector<char>& stack,
	                     Expect& expect) {
		switch (expect) {
			case Expect::Value:
			case Expect::ValueOrEnd:
				return jsonValueStep(text, at, stack, expect);
			case Expect::KeyOrEnd:
				if (text[at] == '}') return closeContainer(text, at, stack, expect);
				/* fallthrough */
			case Expect::Key:
				expect = Expect::Colon;
				return scanJsonString(text, at);
			case Expect::Colon:
				expect = Expect::Value;
				return text[at++] == ':';
			case Expect::CommaOrEnd:
				if (text[at] != ',') return closeContainer(text, at, stack, expect);
				++at;
				expect = stack.back() == '{' ? Expect::Key : Expect::Value;
				return true;
			default:
				return false;
		}
	}

	static bool isJson(const std::string& text) {
		std::size_t at = skipSpace(text, 0);
		if (at >= text.size() || (text[at] != '{' && text[at] != '[')) return false;

		std::vector<char> stack;
		Expect expect = Expect::Value;
		while ((at = skipSpace(text, at)) < text.size()) {
			if (!jsonStep(text, at, stack, expect)) return false;
		}

		return expect == Expect::Done;
	}

	static std::vector<std::string> firstLines(const std::string& text, std::size_t count) {
		std::vector<std::string> lines;
		std::size_t at = 0;
		while (at < text.size() && lines.size() < count) {
			const std::size_t end = std::min(text.find('\n', at), text.size());
			std::string line = text.substr(at, end - at);
			if (!line.empty() && line.back() == '\r') line.pop_back();
			lines.push_back(line);
			at = end + 1;
		}

		return lines;
	}

	static bool anyLineStarts(const std::vector<std::string>& lines, const char* prefix) {
		for (const std::string& line : lines) {
			if (startsAt(lowered(line), 0, prefix)) return true;
		}

		return false;
	}

	static bool detectBatch(const std::vector<std::string>& lines, TextKind& kind) {
		const std::string first = lines.empty() ? "" : lowered(lines[0]);
		if (!startsAt(first, 0, "@echo off") && !startsAt(first, 0, "@echo on")) return false;

		kind.label = "DOS batch file";
		kind.mime = "text/x-msdos-batch";
		return true;
	}

	static bool detectPowerShell(const std::string& text, const std::vector<std::string>& lines,
	                             TextKind& kind) {
		static const char* const kStarts[] = {
			"#requires", "[cmdletbinding(", "param(", "param (",
		};
		bool found = text.find("$PSScriptRoot") != std::string::npos
			|| text.find("Write-Host ") != std::string::npos;
		for (const char* start : kStarts) found = found || anyLineStarts(lines, start);
		if (!found) return false;

		kind.label = "PowerShell script";
		return true;
	}

	static bool detectMakefile(const std::vector<std::string>& lines, TextKind& kind) {
		if (!anyLineStarts(lines, ".phony:") && !anyLineStarts(lines, ".phony :")) return false;

		kind.label = "makefile script";
		kind.mime = "text/x-makefile";
		return true;
	}

	static bool detectCSource(const std::string& text, const std::vector<std::string>& lines,
	                          TextKind& kind) {
		const bool c_like = anyLineStarts(lines, "#include <")
			|| anyLineStarts(lines, "#include \"");
		if (!c_like) return false;

		static const char* const kCppMarks[] = {
			"namespace ", "template <", "template<", "std::", "class ", "#include <iostream>",
		};
		bool cpp = false;
		for (const char* mark : kCppMarks) cpp = cpp || text.find(mark) != std::string::npos;
		kind.label = cpp ? "C++ source" : "C source";
		kind.mime = cpp ? "text/x-c++" : "text/x-c";
		return true;
	}

	static bool detectKind(const std::string& text, bool complete, TextKind& kind) {
		if (detectShebang(text, kind) || detectPem(text, kind)) return true;
		if (complete && isJson(text)) {
			kind.label = "JSON text data";
			kind.mime = "application/json";
			kind.whole = true;
			return true;
		}

		if (detectMarkup(text, kind)) return true;

		const std::vector<std::string> lines = firstLines(text, 200);
		return detectBatch(lines, kind) || detectCSource(text, lines, kind)
			|| detectMakefile(lines, kind) || detectPowerShell(text, lines, kind);
	}

	static std::string describeText(const TextKind& kind, Encoding encoding,
	                                const TextFacts& facts) {
		if (kind.whole) return kind.label;

		const std::string executable = kind.executable ? " executable" : "";
		if (kind.bare) return kind.label + " text" + executable + factsText(facts);

		const std::string body = std::string(encodingLabel(encoding)) + " text" + executable;
		if (kind.label.empty()) return body + factsText(facts);
		return kind.label + ", " + body + factsText(facts);
	}

	bool identifyText(const FileSample& sample, MagicResult& result) {
		Encoding encoding = Encoding::Ascii;
		std::u32string codes;
		if (!decodeText(sample, encoding, codes)) return false;

		const bool complete = sample.size <= sample.bytes.size();
		TextKind kind;
		detectKind(asUtf8(codes), complete, kind);
		result.description = describeText(kind, encoding, gatherFacts(codes));
		result.mime_type = kind.mime;
		result.encoding = charsetName(encoding);
		return true;
	}

}  // namespace wbsh
