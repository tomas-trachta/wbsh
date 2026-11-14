/**
 * @file coreutils_encoding.cpp
 * @brief Encoding coreutils: base64, base32, uuencode, uudecode, xxd.
 */

#include "coreutils_internal.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "base64.h"
#include "executor.h"
#include "numparse.h"
#include "strscan.h"

namespace wbsh {

	static const std::size_t kBase64DefaultWrap = 76;
	static const int kXxdDefaultColumns = 16;
	static const int kXxdPlainLineHexDigits = 60;

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

	static const char* const kBase32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
	static const std::size_t kUuLineBytes = 45;
	static const std::size_t kUuBase64Wrap = 60;

	static std::string base32Encode(const std::vector<unsigned char>& in) {
		std::string out;
		for (std::size_t i = 0; i < in.size(); i += 5) {
			const std::size_t take = (std::min)(static_cast<std::size_t>(5), in.size() - i);
			unsigned long long group = 0;
			for (std::size_t k = 0; k < 5; ++k) {
				group = (group << 8) | (k < take ? in[i + k] : 0);
			}

			const std::size_t significant = (take * 8 + 4) / 5;
			for (std::size_t k = 0; k < 8; ++k) {
				const unsigned index = static_cast<unsigned>((group >> (35 - 5 * k)) & 0x1F);
				out.push_back(k < significant ? kBase32Alphabet[index] : '=');
			}
		}

		return out;
	}

	static int base32Value(char c) {
		const int upper = std::toupper(static_cast<unsigned char>(c));
		const char* found = std::strchr(kBase32Alphabet, upper);
		if (found == nullptr || c == '\0') return -1;
		return static_cast<int>(found - kBase32Alphabet);
	}

	static bool base32Decode(const std::string& in, bool ignore_garbage,
	                         std::vector<unsigned char>& out) {
		unsigned long long buffer = 0;
		int bits = 0;
		for (const char c : in) {
			if (c == '=' || std::isspace(static_cast<unsigned char>(c)) != 0) continue;

			const int value = base32Value(c);
			if (value < 0 && ignore_garbage) continue;
			if (value < 0) return false;

			buffer = (buffer << 5) | static_cast<unsigned>(value);
			bits += 5;
			if (bits < 8) continue;

			bits -= 8;
			out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
		}

		return true;
	}

	static int builtin_base32(Executor& exec, const std::vector<std::string>& args) {
		const Base64Options options = parseBase64Args(args);
		bool ignore_garbage = false;
		for (const std::string& arg : args) {
			if (arg == "-i" || arg == "--ignore-garbage") ignore_garbage = true;
		}

		std::vector<unsigned char> bytes;
		if (!readBase64Input(exec, options.file, bytes)) return 1;

		if (!options.decode) {
			writeWrapped(base32Encode(bytes), options.wrap);
			std::fflush(stdout);
			return 0;
		}

		std::vector<unsigned char> decoded;
		if (!base32Decode(std::string(bytes.begin(), bytes.end()), ignore_garbage, decoded)) {
			perr("base32", "invalid input");
			return 1;
		}

		std::fwrite(decoded.data(), 1, decoded.size(), stdout);
		std::fflush(stdout);
		return 0;
	}

	static char uuChar(unsigned value) {
		return value == 0 ? '`' : static_cast<char>(' ' + value);
	}

	static void uuEncodeLine(const std::vector<unsigned char>& bytes, std::size_t at,
	                         std::size_t take) {
		std::string line(1, uuChar(static_cast<unsigned>(take)));
		for (std::size_t k = 0; k < take; k += 3) {
			const unsigned b0 = bytes[at + k];
			const unsigned b1 = k + 1 < take ? bytes[at + k + 1] : 0;
			const unsigned b2 = k + 2 < take ? bytes[at + k + 2] : 0;
			line.push_back(uuChar(b0 >> 2));
			line.push_back(uuChar(((b0 << 4) | (b1 >> 4)) & 0x3F));
			line.push_back(uuChar(((b1 << 2) | (b2 >> 6)) & 0x3F));
			line.push_back(uuChar(b2 & 0x3F));
		}

		std::printf("%s\n", line.c_str());
	}

	static int builtin_uuencode(Executor& exec, const std::vector<std::string>& args) {
		bool base64 = false;
		std::vector<std::string> operands;
		for (const std::string& arg : args) {
			if (arg == "-m" || arg == "--base64") base64 = true;
			else                                  operands.push_back(arg);
		}

		if (operands.empty() || operands.size() > 2) {
			perr("uuencode", "usage: uuencode [-m] [INFILE] STORED_NAME");
			return 1;
		}

		std::vector<unsigned char> bytes;
		const std::string input = operands.size() == 2 ? operands[0] : "-";
		if (!readInputBytes(exec, "uuencode", input, bytes)) return 1;

		const std::string& name = operands.back();
		std::printf("begin%s 644 %s\n", base64 ? "-base64" : "", name.c_str());
		if (base64) {
			writeWrapped(base64Encode(bytes), kUuBase64Wrap);
			std::printf("====\n");
			std::fflush(stdout);
			return 0;
		}

		for (std::size_t at = 0; at < bytes.size(); at += kUuLineBytes) {
			uuEncodeLine(bytes, at, (std::min)(kUuLineBytes, bytes.size() - at));
		}

		std::printf("`\nend\n");
		std::fflush(stdout);
		return 0;
	}

	static unsigned uuValue(char c) {
		return static_cast<unsigned>(c - ' ') & 0x3F;
	}

	static void uuDecodeLine(const std::string& line, std::vector<unsigned char>& out) {
		if (line.empty()) return;

		const std::size_t length = uuValue(line[0]);
		std::size_t produced = 0;
		for (std::size_t k = 1; k < line.size() && produced < length; k += 4) {
			unsigned quad[4] = { 0, 0, 0, 0 };
			for (std::size_t q = 0; q < 4; ++q) {
				if (k + q < line.size()) quad[q] = uuValue(line[k + q]);
			}

			const unsigned char decoded[3] = {
				static_cast<unsigned char>((quad[0] << 2) | (quad[1] >> 4)),
				static_cast<unsigned char>((quad[1] << 4) | (quad[2] >> 2)),
				static_cast<unsigned char>((quad[2] << 6) | quad[3]),
			};
			for (std::size_t b = 0; b < 3 && produced < length; ++b, ++produced) {
				out.push_back(decoded[b]);
			}
		}
	}

	static bool parseUuHeader(const std::string& line, bool& base64, std::string& name) {
		const bool traditional = line.compare(0, 6, "begin ") == 0;
		base64 = line.compare(0, 13, "begin-base64 ") == 0;
		if (!traditional && !base64) return false;

		const std::size_t mode_start = line.find(' ');
		const std::size_t name_start = line.find(' ', mode_start + 1);
		if (name_start == std::string::npos) return false;

		name = line.substr(name_start + 1);
		return !name.empty();
	}

	static bool decodeUuBody(const std::vector<std::string>& lines, std::size_t first, bool base64,
	                         std::vector<unsigned char>& out) {
		std::string encoded;
		for (std::size_t i = first; i < lines.size(); ++i) {
			const std::string& line = lines[i];
			if (base64 && line == "====") break;
			if (!base64 && line == "end") break;
			if (base64) encoded += line;
			else        uuDecodeLine(line, out);
		}

		if (!base64) return true;

		bool ok = false;
		out = base64Decode(encoded, ok);
		return ok;
	}

	static bool writeDecodedFile(Executor& exec, const std::string& target,
	                             const std::vector<unsigned char>& data) {
		if (target == "-" || target == "/dev/stdout") {
			std::fwrite(data.data(), 1, data.size(), stdout);
			std::fflush(stdout);
			return true;
		}

		std::FILE* out = fopenNative(exec, target, "wb");
		if (out == nullptr) {
			perr("uudecode", target + ": " + std::strerror(errno));
			return false;
		}

		std::fwrite(data.data(), 1, data.size(), out);
		std::fclose(out);
		return true;
	}

	static int builtin_uudecode(Executor& exec, const std::vector<std::string>& args) {
		std::string output;
		std::string input = "-";
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (args[i] == "-o" && i + 1 < args.size()) output = args[++i];
			else                                        input = args[i];
		}

		std::vector<std::string> lines;
		if (!readAllLines(exec, input, lines)) {
			perr("uudecode", input + ": " + std::strerror(errno));
			return 1;
		}

		for (std::size_t i = 0; i < lines.size(); ++i) {
			bool base64 = false;
			std::string name;
			if (!parseUuHeader(lines[i], base64, name)) continue;

			std::vector<unsigned char> data;
			if (!decodeUuBody(lines, i + 1, base64, data)) {
				perr("uudecode", "invalid input");
				return 1;
			}

			return writeDecodedFile(exec, output.empty() ? name : output, data) ? 0 : 1;
		}

		perr("uudecode", "no 'begin' line");
		return 1;
	}

	void registerEncodingBuiltins(Executor& exec) {
		exec.registerBuiltin("xxd",      builtin_xxd);
		exec.registerBuiltin("base64",   builtin_base64);
		exec.registerBuiltin("base32",   builtin_base32);
		exec.registerBuiltin("uuencode", builtin_uuencode);
		exec.registerBuiltin("uudecode", builtin_uudecode);
	}

}  // namespace wbsh
