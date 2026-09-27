/**
 * @file coreutils_encoding.cpp
 * @brief Byte-dump / encoding coreutils: base64, xxd, od.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "executor.h"
#include "numparse.h"
#include "strscan.h"

namespace wbsh {

	static const char* const kBase64Alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	static const std::size_t kBase64DefaultWrap = 76;
	static const int kXxdDefaultColumns = 16;
	static const int kXxdPlainLineHexDigits = 60;
	static const std::size_t kOdBytesPerRow = 16;

	static std::vector<unsigned char> readAllBytes(FILE* stream) {
		std::vector<unsigned char> bytes;
		int c;
		while ((c = std::fgetc(stream)) != EOF) bytes.push_back(static_cast<unsigned char>(c));
		return bytes;
	}

	static bool readInputBytes(Executor& exec, const char* tool, const std::string& path,
			std::vector<unsigned char>& bytes) {
		if (path.empty() || path == "-") {
			bytes = readAllBytes(stdin);
			return true;
		}

		FILE* file = fopenNative(exec, path, "rb");
		if (file == nullptr) {
			perr(tool, path, std::error_code(errno, std::system_category()));
			return false;
		}

		bytes = readAllBytes(file);
		std::fclose(file);
		return true;
	}

	static void appendBase64Group(std::string& out, const std::vector<unsigned char>& in,
			std::size_t i) {
		const std::size_t size = in.size();
		unsigned int triple = static_cast<unsigned int>(in[i]) << 16;
		if (i + 1 < size) triple |= static_cast<unsigned int>(in[i + 1]) << 8;
		if (i + 2 < size) triple |= static_cast<unsigned int>(in[i + 2]);

		out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
		out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
		out.push_back((i + 1 < size) ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
		out.push_back((i + 2 < size) ? kBase64Alphabet[triple & 0x3F] : '=');
	}

	static std::string base64Encode(const std::vector<unsigned char>& in) {
		std::string out;
		out.reserve(((in.size() + 2) / 3) * 4);
		for (std::size_t i = 0; i < in.size(); i += 3) appendBase64Group(out, in, i);
		return out;
	}

	static void buildBase64DecodeTable(int (&table)[256]) {
		for (int i = 0; i < 256; ++i) table[i] = -1;
		for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
	}

	static bool isBase64Whitespace(char c) {
		return c == '\n' || c == '\r' || c == ' ' || c == '\t';
	}

	static std::vector<unsigned char> base64Decode(const std::string& in, bool& ok) {
		ok = true;
		int table[256];
		buildBase64DecodeTable(table);

		std::vector<unsigned char> out;
		int accumulator = 0;
		int bits = 0;
		for (char c : in) {
			if (isBase64Whitespace(c)) continue;
			if (c == '=') break;

			const int digit = table[static_cast<unsigned char>(c)];
			if (digit < 0) {
				ok = false;
				return out;
			}

			accumulator = (accumulator << 6) | digit;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
			}
		}

		return out;
	}

	struct Base64Options {
		bool decode = false;
		std::string file = "-";
		std::size_t wrap = kBase64DefaultWrap;
	};

	static Base64Options parseBase64Args(const std::vector<std::string>& args) {
		Base64Options options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-d" || arg == "--decode") {
				options.decode = true;
				continue;
			}

			if (arg == "-w" && i + 1 < args.size()) {
				unsigned long wrap = 0;
				if (parseUL(args[++i], wrap)) options.wrap = wrap;
				continue;
			}

			if (!arg.empty() && arg[0] == '-' && arg != "-") continue;
			options.file = arg;
		}

		return options;
	}

	static bool readBase64Input(Executor& exec, const std::string& file,
			std::vector<unsigned char>& bytes) {
		FILE* stream = (file == "-") ? stdin : fopenNative(exec, file, "rb");
		if (stream == nullptr) {
			std::fprintf(stderr, "wbsh: base64: %s: %s\n",
				file.c_str(), std::strerror(errno));
			return false;
		}

		bytes = readAllBytes(stream);
		if (stream != stdin) std::fclose(stream);
		return true;
	}

	static void writeWrapped(const std::string& encoded, std::size_t wrap) {
		if (wrap == 0) {
			std::fwrite(encoded.data(), 1, encoded.size(), stdout);
			std::fputc('\n', stdout);
			return;
		}

		for (std::size_t i = 0; i < encoded.size(); i += wrap) {
			const std::size_t take = (std::min)(wrap, encoded.size() - i);
			std::fwrite(encoded.data() + i, 1, take, stdout);
			std::fputc('\n', stdout);
		}
	}

	static int builtin_base64(Executor& exec, const std::vector<std::string>& args) {
		const Base64Options options = parseBase64Args(args);

		std::vector<unsigned char> bytes;
		if (!readBase64Input(exec, options.file, bytes)) return 1;

		if (options.decode) {
			bool ok;
			const std::vector<unsigned char> decoded =
				base64Decode(std::string(bytes.begin(), bytes.end()), ok);
			if (!ok) {
				perr("base64", "invalid input");
				return 1;
			}

			std::fwrite(decoded.data(), 1, decoded.size(), stdout);
		} else {
			writeWrapped(base64Encode(bytes), options.wrap);
		}

		std::fflush(stdout);
		return 0;
	}

	struct XxdOptions {
		bool plain = false;
		bool reverse = false;
		int cols = kXxdDefaultColumns;
		std::string path;
	};

	static bool parseXxdArgs(const std::vector<std::string>& args, XxdOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-p" || arg == "--plain") {
				options.plain = true;
			} else if (arg == "-r" || arg == "--revert") {
				options.reverse = true;
			} else if (arg == "-c" && i + 1 < args.size()) {
				parseInt(args[++i], options.cols);
			} else if (!arg.empty() && arg[0] == '-' && arg != "-") {
				perr("xxd", "unknown option: " + arg);
				return false;
			} else if (options.path.empty()) {
				options.path = arg;
			}
		}

		return true;
	}

	static int hexDigitValue(char c) {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return 10 + c - 'a';
		if (c >= 'A' && c <= 'F') return 10 + c - 'A';
		return -1;
	}

	static std::string xxdHexColumn(const std::string& line, bool plain) {
		if (plain) return line;

		StrScan in(line);
		in.skipPast(":");  /* offset column; tolerate lines without one */
		std::string hex;
		if (!in.readUpTo("  ", hex)) hex = in.rest();
		return hex;
	}

	static void xxdRevertLine(const std::string& line, bool plain,
			std::vector<unsigned char>& out) {
		const std::string hex = xxdHexColumn(line, plain);

		unsigned current = 0;
		int half = 0;
		for (char c : hex) {
			const int digit = hexDigitValue(c);
			if (digit < 0) continue;

			current = (current << 4) | static_cast<unsigned>(digit);
			half++;
			if (half == 2) {
				out.push_back(static_cast<unsigned char>(current));
				current = 0;
				half = 0;
			}
		}
	}

	static int xxdEmitRevert(const std::vector<unsigned char>& bytes, bool plain) {
		std::vector<unsigned char> out;
		std::string line;
		for (unsigned char c : bytes) {
			if (c != '\n') {
				line.push_back(static_cast<char>(c));
				continue;
			}

			xxdRevertLine(line, plain, out);
			line.clear();
		}

		if (!line.empty()) xxdRevertLine(line, plain, out);
		std::fwrite(out.data(), 1, out.size(), stdout);
		return 0;
	}

	static int xxdEmitPlain(const std::vector<unsigned char>& bytes, int cols) {
		const int per_line = (cols > 0) ? cols * 2 : kXxdPlainLineHexDigits;
		int written = 0;
		for (unsigned char c : bytes) {
			std::printf("%02x", c);
			written += 2;
			if (written >= per_line) {
				std::printf("\n");
				written = 0;
			}
		}

		if (written != 0) std::printf("\n");
		return 0;
	}

	static void xxdEmitRow(const std::vector<unsigned char>& bytes, std::size_t addr,
			std::size_t end, std::size_t cols) {
		std::printf("%08zx:", addr);
		for (std::size_t i = addr; i < addr + cols; ++i) {
			if ((i - addr) % 2 == 0) std::printf(" ");
			if (i < end) std::printf("%02x", bytes[i]);
			else         std::printf("  ");
		}

		std::printf("  ");
		for (std::size_t i = addr; i < end; ++i) {
			const unsigned char c = bytes[i];
			std::putchar((c >= 32 && c < 127) ? c : '.');
		}

		std::printf("\n");
	}

	static int xxdEmitDefault(const std::vector<unsigned char>& bytes, int cols) {
		const std::size_t row_width = static_cast<std::size_t>(cols);
		std::size_t addr = 0;
		while (addr < bytes.size()) {
			const std::size_t end = (std::min)(bytes.size(), addr + row_width);
			xxdEmitRow(bytes, addr, end, row_width);
			addr = end;
		}

		return 0;
	}

	static int builtin_xxd(Executor& exec, const std::vector<std::string>& args) {
		XxdOptions options;
		if (!parseXxdArgs(args, options)) return 1;

		std::vector<unsigned char> bytes;
		if (!readInputBytes(exec, "xxd", options.path, bytes)) return 1;

		if (options.reverse) return xxdEmitRevert(bytes, options.plain);
		if (options.plain)   return xxdEmitPlain(bytes, options.cols);
		return xxdEmitDefault(bytes, options.cols);
	}

	struct OdOptions {
		char format = 'o';
		char addr_format = 'o';
		std::string path;
	};

	static bool isOdFormat(char c) {
		return c == 'x' || c == 'd' || c == 'o' || c == 'c';
	}

	static bool parseOdArgs(const std::vector<std::string>& args, OdOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-c") {
				options.format = 'c';
			} else if (arg == "-x" || arg == "-h") {
				options.format = 'x';
			} else if (arg == "-d") {
				options.format = 'd';
			} else if (arg == "-o") {
				options.format = 'o';
			} else if (arg == "-A" && i + 1 < args.size()) {
				const std::string& radix = args[++i];
				if (!radix.empty()) options.addr_format = radix[0];
			} else if (arg == "-t" && i + 1 < args.size()) {
				const std::string& type = args[++i];
				if (!type.empty() && isOdFormat(type[0])) options.format = type[0];
			} else if (!arg.empty() && arg[0] == '-' && arg != "-") {
				perr("od", "unknown option: " + arg);
				return false;
			} else if (options.path.empty()) {
				options.path = arg;
			}
		}

		return true;
	}

	static void odPrintAddr(std::size_t addr, char addr_format, bool with_newline) {
		const char* newline = with_newline ? "\n" : "";
		switch (addr_format) {
		case 'd': std::printf("%07zu%s", addr, newline); return;
		case 'x': std::printf("%07zx%s", addr, newline); return;
		case 'n': return;
		default:  std::printf("%07zo%s", addr, newline); return;
		}
	}

	static const char* odEscapeName(unsigned char byte) {
		switch (byte) {
		case '\0': return "\\0";
		case '\a': return "\\a";
		case '\b': return "\\b";
		case '\t': return "\\t";
		case '\n': return "\\n";
		case '\v': return "\\v";
		case '\f': return "\\f";
		case '\r': return "\\r";
		default:   return nullptr;
		}
	}

	static void odEmitCharCell(unsigned char byte) {
		const char* escape = odEscapeName(byte);
		if (escape != nullptr) std::printf("  %s", escape);
		else if (byte >= 32 && byte < 127) std::printf("   %c", byte);
		else std::printf(" %03o", byte);
	}

	static const char* odNumericSpec(char format) {
		if (format == 'x') return " %02x";
		if (format == 'd') return " %3u";
		return " %03o";
	}

	static void odEmitRow(const std::vector<unsigned char>& bytes,
			std::size_t addr, std::size_t end, char format) {
		if (format == 'c') {
			for (std::size_t i = addr; i < end; ++i) odEmitCharCell(bytes[i]);
			return;
		}

		const char* spec = odNumericSpec(format);
		for (std::size_t i = addr; i < end; ++i) std::printf(spec, bytes[i]);
	}

	static int builtin_od(Executor& exec, const std::vector<std::string>& args) {
		OdOptions options;
		if (!parseOdArgs(args, options)) return 1;

		std::vector<unsigned char> bytes;
		if (!readInputBytes(exec, "od", options.path, bytes)) return 1;

		std::size_t addr = 0;
		while (addr < bytes.size()) {
			odPrintAddr(addr, options.addr_format, false);
			const std::size_t end = (std::min)(bytes.size(), addr + kOdBytesPerRow);
			odEmitRow(bytes, addr, end, options.format);
			std::printf("\n");
			addr = end;
		}

		odPrintAddr(bytes.size(), options.addr_format, true);
		return 0;
	}

	void registerEncodingBuiltins(Executor& exec) {
		exec.registerBuiltin("xxd",    builtin_xxd);
		exec.registerBuiltin("od",     builtin_od);
		exec.registerBuiltin("base64", builtin_base64);
	}

}  // namespace wbsh
