/**
 * @file coreutils_dump.cpp
 * @brief Byte dumps: od (POSIX / GNU types) and hexdump / hd (the BSD
 *        canned formats).
 *
 * Input is streamed row by row across every named file, so `-N` on an
 * endless source such as /dev/urandom stops when it should.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"

namespace wbsh {

	namespace dump_detail {

		enum class OdKind {
			Named,
			Char,
			Signed,
			Unsigned,
			Octal,
			Hex,
			Float,
		};

		struct OdType {
			OdKind kind = OdKind::Octal;
			std::size_t size = 2;
			bool show_text = false;
			int width = 6;
		};

		struct DumpOptions {
			char radix = 'o';
			unsigned long long skip = 0;
			unsigned long long limit = ~0ULL;
			bool all_rows = false;
			std::size_t row_bytes = 16;
			std::vector<OdType> types;
			std::vector<char> hex_formats;
			std::vector<std::string> files;
		};

		struct DumpInput {
			Executor* exec = nullptr;
			std::vector<std::string> files;
			std::size_t next_file = 0;
			std::FILE* current = nullptr;
			unsigned long long remaining = ~0ULL;
			bool failed = false;
		};

	}  // namespace dump_detail

	using dump_detail::DumpInput;
	using dump_detail::DumpOptions;
	using dump_detail::OdKind;
	using dump_detail::OdType;

	static const char* const kNamedChars[] = {
		"nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
		"bs",  "ht",  "nl",  "vt",  "ff",  "cr",  "so",  "si",
		"dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
		"can", "em",  "sub", "esc", "fs",  "gs",  "rs",  "us",
	};

	static void closeCurrent(DumpInput& input) {
		if (input.current != nullptr && input.current != stdin) std::fclose(input.current);
		input.current = nullptr;
	}

	static bool openNextFile(DumpInput& input, const char* cmd) {
		while (input.next_file < input.files.size()) {
			const std::string& name = input.files[input.next_file++];
			input.current = name == "-" ? stdin : fopenNative(*input.exec, name, "rb");
			if (input.current != nullptr) return true;

			perr(cmd, name + ": " + std::strerror(errno));
			input.failed = true;
		}

		return false;
	}

	static std::size_t readInput(DumpInput& input, unsigned char* buffer, std::size_t wanted,
	                             const char* cmd) {
		if (wanted > input.remaining) wanted = static_cast<std::size_t>(input.remaining);
		std::size_t total = 0;
		while (total < wanted) {
			if (input.current == nullptr && !openNextFile(input, cmd)) break;

			const std::size_t got = std::fread(buffer + total, 1, wanted - total, input.current);
			total += got;
			if (got == 0) closeCurrent(input);
		}

		input.remaining -= total;
		return total;
	}

	static void skipInput(DumpInput& input, unsigned long long count, const char* cmd) {
		std::vector<unsigned char> chunk(64 * 1024);
		const unsigned long long saved_limit = input.remaining;
		input.remaining = ~0ULL;
		while (count > 0) {
			const std::size_t want = count < chunk.size() ? static_cast<std::size_t>(count)
				: chunk.size();
			const std::size_t got = readInput(input, chunk.data(), want, cmd);
			if (got == 0) break;
			count -= got;
		}

		input.remaining = saved_limit;
	}

	static int odNaturalWidth(OdKind kind, std::size_t size) {
		static const int kOctal[]    = { 3, 6, 11, 22 };
		static const int kUnsigned[] = { 3, 5, 10, 20 };
		static const int kSigned[]   = { 4, 6, 11, 20 };
		const std::size_t index = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
		switch (kind) {
		case OdKind::Named:
		case OdKind::Char:     return 3;
		case OdKind::Octal:    return kOctal[index];
		case OdKind::Unsigned: return kUnsigned[index];
		case OdKind::Signed:   return kSigned[index];
		case OdKind::Hex:      return static_cast<int>(size * 2);
		case OdKind::Float:    return size == 4 ? 14 : 23;
		default:               return 3;
		}
	}

	static bool parseOdSize(const std::string& spec, std::size_t& at, OdKind kind,
	                        std::size_t& size) {
		const bool is_float = kind == OdKind::Float;
		size = is_float ? 8 : 4;
		if (at >= spec.size()) return true;

		const char letter = spec[at];
		if (letter >= '0' && letter <= '9') size = 0;
		while (at < spec.size() && spec[at] >= '0' && spec[at] <= '9') {
			size = size * 10 + static_cast<std::size_t>(spec[at++] - '0');
		}

		const char* integer_letter = is_float ? nullptr : std::strchr("CSIL", letter);
		const char* float_letter = is_float ? std::strchr("FDL", letter) : nullptr;
		if (integer_letter != nullptr && letter != '\0') {
			static const std::size_t kIntegerSizes[] = { 1, 2, 4, 8 };
			size = kIntegerSizes[integer_letter - "CSIL"];
			++at;
		}

		if (float_letter != nullptr && letter != '\0') {
			size = letter == 'F' ? 4 : 8;
			++at;
		}

		if (is_float) return size == 4 || size == 8;
		return size == 1 || size == 2 || size == 4 || size == 8;
	}

	static bool odKindForLetter(char letter, OdKind& kind) {
		switch (letter) {
		case 'a': kind = OdKind::Named;    return true;
		case 'c': kind = OdKind::Char;     return true;
		case 'd': kind = OdKind::Signed;   return true;
		case 'u': kind = OdKind::Unsigned; return true;
		case 'o': kind = OdKind::Octal;    return true;
		case 'x': kind = OdKind::Hex;      return true;
		case 'f': kind = OdKind::Float;    return true;
		default:  return false;
		}
	}

	static bool parseOdTypeSpec(const std::string& spec, std::vector<OdType>& types) {
		std::size_t at = 0;
		while (at < spec.size()) {
			OdType type;
			if (!odKindForLetter(spec[at++], type.kind)) return false;

			const bool textual = type.kind == OdKind::Named || type.kind == OdKind::Char;
			type.size = 1;
			if (!textual && !parseOdSize(spec, at, type.kind, type.size)) return false;
			if (at < spec.size() && spec[at] == 'z') {
				type.show_text = true;
				++at;
			}

			type.width = odNaturalWidth(type.kind, type.size);
			types.push_back(type);
		}

		return !spec.empty();
	}

	static bool traditionalOdType(char flag, std::string& spec) {
		switch (flag) {
		case 'a': spec = "a";  return true;
		case 'b': spec = "o1"; return true;
		case 'c': spec = "c";  return true;
		case 'd': spec = "u2"; return true;
		case 'D': spec = "u4"; return true;
		case 'f': spec = "f4"; return true;
		case 'F': spec = "f8"; return true;
		case 'i': spec = "d2"; return true;
		case 's': spec = "d2"; return true;
		case 'I': spec = "d8"; return true;
		case 'l': spec = "d4"; return true;
		case 'L': spec = "d8"; return true;
		case 'o': spec = "o2"; return true;
		case 'O': spec = "o4"; return true;
		case 'x': spec = "x2"; return true;
		case 'h': spec = "x2"; return true;
		case 'X': spec = "x4"; return true;
		default:  return false;
		}
	}

	static bool takeDumpValue(const std::vector<std::string>& args, std::size_t& i, char flag,
	                          std::string& value) {
		const std::string& arg = args[i];
		if (arg.size() < 2 || arg[0] != '-' || arg[1] != flag) return false;
		if (arg.size() > 2) {
			value = arg.substr(2);
			return true;
		}

		if (i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool applyOdValueOption(const std::vector<std::string>& args, std::size_t& i,
	                               DumpOptions& options, bool& valid) {
		std::string value;
		if (takeDumpValue(args, i, 'A', value)) {
			options.radix = value.empty() ? 'o' : value[0];
			valid = std::strchr("doxn", options.radix) != nullptr;
			return true;
		}

		if (takeDumpValue(args, i, 't', value)) {
			valid = parseOdTypeSpec(value, options.types);
			return true;
		}

		if (takeDumpValue(args, i, 'j', value)) {
			valid = parseByteSize(value, options.skip);
			return true;
		}

		if (!takeDumpValue(args, i, 'N', value)) return false;
		valid = parseByteSize(value, options.limit);
		return true;
	}

	static bool applyOdWidth(const std::string& arg, DumpOptions& options) {
		if (arg == "-w") {
			options.row_bytes = 32;
			return true;
		}

		unsigned long long width = 0;
		if (!parseByteSize(arg.substr(2), width) || width == 0) return false;
		options.row_bytes = static_cast<std::size_t>(width);
		return true;
	}

	static bool applyOdFlags(const std::string& arg, DumpOptions& options) {
		if (arg.compare(0, 2, "-w") == 0) return applyOdWidth(arg, options);
		for (std::size_t k = 1; k < arg.size(); ++k) {
			std::string spec;
			if (arg[k] == 'v') {
				options.all_rows = true;
				continue;
			}

			if (!traditionalOdType(arg[k], spec)) return false;
			parseOdTypeSpec(spec, options.types);
		}

		return true;
	}

	static bool parseOdArgs(const std::vector<std::string>& args, DumpOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			bool valid = true;
			if (applyOdValueOption(args, i, options, valid)) {
				if (valid) continue;
				perr("od", "invalid argument '" + arg + "'");
				return false;
			}

			if (arg == "--") continue;
			if (applyOdFlags(arg, options)) continue;
			perr("od", "unknown option: " + arg);
			return false;
		}

		if (options.types.empty()) parseOdTypeSpec("o2", options.types);
		if (options.files.empty()) options.files.push_back("-");
		return true;
	}

	static std::string odAddress(unsigned long long offset, char radix) {
		char text[32];
		switch (radix) {
		case 'd': std::snprintf(text, sizeof(text), "%07llu", offset); break;
		case 'x': std::snprintf(text, sizeof(text), "%06llx", offset); break;
		case 'n': text[0] = '\0';                                       break;
		default:  std::snprintf(text, sizeof(text), "%07llo", offset); break;
		}

		return text;
	}

	static unsigned long long loadLittleEndian(const unsigned char* bytes, std::size_t size) {
		unsigned long long value = 0;
		for (std::size_t k = size; k > 0; --k) value = (value << 8) | bytes[k - 1];
		return value;
	}

	static long long signExtend(unsigned long long value, std::size_t size) {
		if (size >= 8) return static_cast<long long>(value);
		const unsigned long long sign = 1ULL << (size * 8 - 1);
		if ((value & sign) == 0) return static_cast<long long>(value);
		return static_cast<long long>(value | ~((sign << 1) - 1));
	}

	static std::string odCharCell(unsigned char byte) {
		switch (byte) {
		case '\0': return "\\0";
		case '\a': return "\\a";
		case '\b': return "\\b";
		case '\t': return "\\t";
		case '\n': return "\\n";
		case '\v': return "\\v";
		case '\f': return "\\f";
		case '\r': return "\\r";
		default:   break;
		}

		char text[8];
		if (byte >= 32 && byte < 127) std::snprintf(text, sizeof(text), "%c", byte);
		else                          std::snprintf(text, sizeof(text), "%03o", byte);
		return text;
	}

	static std::string odNamedCell(unsigned char byte) {
		const unsigned char low = byte & 0x7F;
		if (low < 32)   return kNamedChars[low];
		if (low == 32)  return "sp";
		if (low == 127) return "del";
		return std::string(1, static_cast<char>(low));
	}

	static double loadFloat(const unsigned char* bytes, std::size_t size) {
		if (size == 4) {
			float value = 0.0f;
			std::memcpy(&value, bytes, sizeof(value));
			return value;
		}

		double value = 0.0;
		std::memcpy(&value, bytes, sizeof(value));
		return value;
	}

	static std::string odNumberCell(const OdType& type, const unsigned char* bytes) {
		const unsigned long long value = loadLittleEndian(bytes, type.size);
		char text[64];
		switch (type.kind) {
		case OdKind::Octal:
			std::snprintf(text, sizeof(text), "%0*llo", type.width, value);
			break;
		case OdKind::Hex:
			std::snprintf(text, sizeof(text), "%0*llx", type.width, value);
			break;
		case OdKind::Unsigned:
			std::snprintf(text, sizeof(text), "%llu", value);
			break;
		case OdKind::Signed:
			std::snprintf(text, sizeof(text), "%lld", signExtend(value, type.size));
			break;
		case OdKind::Float:
			std::snprintf(text, sizeof(text), type.size == 4 ? "%.7e" : "%.16e",
				loadFloat(bytes, type.size));
			break;
		default:
			text[0] = '\0';
			break;
		}

		return text;
	}

	static std::string odCell(const OdType& type, const unsigned char* bytes) {
		if (type.kind == OdKind::Char)  return odCharCell(bytes[0]);
		if (type.kind == OdKind::Named) return odNamedCell(bytes[0]);
		return odNumberCell(type, bytes);
	}

	static int odFieldWidth(const OdType& type, const std::vector<OdType>& types) {
		if (types.size() == 1) return type.width;

		std::size_t per_byte = 1;
		for (const OdType& other : types) {
			const std::size_t needed = (static_cast<std::size_t>(other.width) + other.size)
				/ other.size;
			per_byte = (std::max)(per_byte, needed);
		}

		return static_cast<int>(type.size * per_byte - 1);
	}

	static std::string odTextColumn(const unsigned char* row, std::size_t got) {
		std::string text = "  >";
		for (std::size_t k = 0; k < got; ++k) {
			text.push_back(row[k] >= 32 && row[k] < 127 ? static_cast<char>(row[k]) : '.');
		}

		return text + "<";
	}

	static void printOdTypeLine(const OdType& type, const DumpOptions& options,
	                            std::vector<unsigned char>& row, std::size_t got) {
		const int width = odFieldWidth(type, options.types);
		std::string line;
		std::size_t at = 0;
		for (; at < got; at += type.size) {
			const std::string cell = odCell(type, row.data() + at);
			line.push_back(' ');
			if (static_cast<int>(cell.size()) < width) line.append(width - cell.size(), ' ');
			line += cell;
		}

		if (type.show_text) {
			const std::size_t full = options.row_bytes / type.size;
			const std::size_t shown = (got + type.size - 1) / type.size;
			line.append((full - shown) * static_cast<std::size_t>(width + 1), ' ');
			line += odTextColumn(row.data(), got);
		}

		std::printf("%s\n", line.c_str());
	}

	static void printOdRow(const DumpOptions& options, unsigned long long offset,
	                       std::vector<unsigned char>& row, std::size_t got) {
		const std::string address = odAddress(offset, options.radix);
		const std::string blank(address.size(), ' ');
		for (std::size_t k = 0; k < options.types.size(); ++k) {
			std::fputs(k == 0 ? address.c_str() : blank.c_str(), stdout);
			printOdTypeLine(options.types[k], options, row, got);
		}
	}

	static std::size_t largestTypeSize(const std::vector<OdType>& types) {
		std::size_t largest = 1;
		for (const OdType& type : types) largest = (std::max)(largest, type.size);
		return largest;
	}

	static int builtin_od(Executor& exec, const std::vector<std::string>& args) {
		DumpOptions options;
		if (!parseOdArgs(args, options)) return 1;

		DumpInput input;
		input.exec = &exec;
		input.files = options.files;
		skipInput(input, options.skip, "od");
		input.remaining = options.limit;

		const std::size_t element = largestTypeSize(options.types);
		std::vector<unsigned char> row(options.row_bytes + element, 0);
		std::vector<unsigned char> previous;
		unsigned long long offset = options.skip;
		bool squeezing = false;
		for (;;) {
			std::fill(row.begin(), row.end(), 0);
			const std::size_t got = readInput(input, row.data(), options.row_bytes, "od");
			if (got == 0 || stdoutFailed()) break;

			const std::vector<unsigned char> current(row.begin(), row.begin() + got);
			const bool full_row = got == options.row_bytes;
			const bool repeat = !options.all_rows && full_row && current == previous;
			if (repeat && !squeezing) std::printf("*\n");
			squeezing = repeat;
			if (!repeat) printOdRow(options, offset, row, got);

			previous = current;
			offset += got;
		}

		closeCurrent(input);
		if (options.radix != 'n') std::printf("%s\n", odAddress(offset, options.radix).c_str());
		std::fflush(stdout);
		return input.failed ? 1 : 0;
	}

	static std::string rightTrimmed(std::string text) {
		while (!text.empty() && text.back() == ' ') text.pop_back();
		return text;
	}

	static std::string hexdumpCharCell(unsigned char byte) {
		const std::string cell = odCharCell(byte);
		return std::string(cell.size() < 3 ? 3 - cell.size() : 0, ' ') + cell;
	}

	static std::string hexdumpCanonicalLine(const unsigned char* row, std::size_t got,
	                                        unsigned long long offset) {
		char text[16];
		std::snprintf(text, sizeof(text), "%08llx ", offset);
		std::string line = text;
		for (std::size_t k = 0; k < 16; ++k) {
			if (k == 8) line.push_back(' ');
			if (k >= got) {
				line += "   ";
				continue;
			}

			std::snprintf(text, sizeof(text), " %02x", row[k]);
			line += text;
		}

		line += "  |";
		for (std::size_t k = 0; k < got; ++k) {
			line.push_back(row[k] >= 32 && row[k] < 127 ? static_cast<char>(row[k]) : '.');
		}

		return line + "|";
	}

	static std::string hexdumpWordCell(char format, const unsigned char* row, std::size_t at,
	                                   std::size_t got) {
		const unsigned low = row[at];
		const unsigned high = at + 1 < got ? row[at + 1] : 0;
		const unsigned word = low | (high << 8);
		char text[16];
		switch (format) {
		case 'd': std::snprintf(text, sizeof(text), "  %05u ", word);  break;
		case 'o': std::snprintf(text, sizeof(text), " %06o ", word);   break;
		case 'x': std::snprintf(text, sizeof(text), "   %04x ", word); break;
		default:  std::snprintf(text, sizeof(text), "%04x ", word);    break;
		}

		return text;
	}

	static std::string hexdumpByteCell(char format, unsigned char byte) {
		if (format == 'c') return hexdumpCharCell(byte) + " ";

		char text[8];
		std::snprintf(text, sizeof(text), "%03o ", byte);
		return text;
	}

	static std::string hexdumpLine(char format, const unsigned char* row, std::size_t got,
	                               unsigned long long offset) {
		if (format == 'C') return hexdumpCanonicalLine(row, got, offset);

		char text[16];
		std::snprintf(text, sizeof(text), "%07llx ", offset);
		std::string line = text;
		const bool bytewise = format == 'b' || format == 'c';
		for (std::size_t at = 0; at < got; at += bytewise ? 1 : 2) {
			if (bytewise) line += hexdumpByteCell(format, row[at]);
			else          line += hexdumpWordCell(format, row, at, got);
		}

		return rightTrimmed(line);
	}

	static bool applyHexdumpFlags(const std::string& cluster, DumpOptions& options) {
		for (std::size_t k = 1; k < cluster.size(); ++k) {
			const char flag = cluster[k];
			if (flag == 'v') {
				options.all_rows = true;
				continue;
			}

			if (std::strchr("Cbcdox", flag) == nullptr) return false;
			options.hex_formats.push_back(flag);
		}

		return true;
	}

	static bool parseHexdumpArgs(const std::vector<std::string>& args, DumpOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			std::string value;
			if (takeDumpValue(args, i, 'n', value)) {
				if (!parseByteSize(value, options.limit)) return false;
				continue;
			}

			if (takeDumpValue(args, i, 's', value)) {
				if (!parseByteSize(value, options.skip)) return false;
				continue;
			}

			if (arg == "-e" || arg == "-f") {
				perr("hexdump", "custom format strings are not supported");
				return false;
			}

			if (arg.size() < 2 || arg[0] != '-') options.files.push_back(arg);
			else if (!applyHexdumpFlags(arg, options)) return false;
		}

		if (options.hex_formats.empty()) options.hex_formats.push_back('h');
		if (options.files.empty()) options.files.push_back("-");
		return true;
	}

	static int runHexdump(Executor& exec, DumpOptions& options) {
		DumpInput input;
		input.exec = &exec;
		input.files = options.files;
		skipInput(input, options.skip, "hexdump");
		input.remaining = options.limit;

		std::vector<unsigned char> row(16, 0);
		std::vector<unsigned char> previous;
		unsigned long long offset = options.skip;
		bool squeezing = false;
		for (;;) {
			const std::size_t got = readInput(input, row.data(), row.size(), "hexdump");
			if (got == 0 || stdoutFailed()) break;

			const std::vector<unsigned char> current(row.begin(), row.begin() + got);
			const bool repeat = !options.all_rows && got == row.size() && current == previous;
			if (repeat && !squeezing) std::printf("*\n");
			squeezing = repeat;
			for (const char format : options.hex_formats) {
				if (repeat) break;
				std::printf("%s\n", hexdumpLine(format, row.data(), got, offset).c_str());
			}

			previous = current;
			offset += got;
		}

		closeCurrent(input);
		std::printf(options.hex_formats[0] == 'C' ? "%08llx\n" : "%07llx\n", offset);
		std::fflush(stdout);
		return input.failed ? 1 : 0;
	}

	static int builtin_hexdump(Executor& exec, const std::vector<std::string>& args) {
		DumpOptions options;
		if (!parseHexdumpArgs(args, options)) {
			perr("hexdump", "usage: hexdump [-bcCdovx] [-n LENGTH] [-s OFFSET] [FILE]...");
			return 1;
		}

		return runHexdump(exec, options);
	}

	static int builtin_hd(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> canonical = { "-C" };
		canonical.insert(canonical.end(), args.begin(), args.end());
		return builtin_hexdump(exec, canonical);
	}

	void registerDumpBuiltins(Executor& exec) {
		exec.registerBuiltin("od",      builtin_od);
		exec.registerBuiltin("hexdump", builtin_hexdump);
		exec.registerBuiltin("hd",      builtin_hd);
	}

}  // namespace wbsh
