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

	const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	static std::string base64Encode(const std::vector<unsigned char>& in) {
		std::string out;
		std::size_t n = in.size();
		out.reserve(((n + 2) / 3) * 4);
		for (std::size_t i = 0; i < n; i += 3) {
			unsigned int v = static_cast<unsigned int>(in[i]) << 16;
			if (i + 1 < n) v |= static_cast<unsigned int>(in[i + 1]) << 8;
			if (i + 2 < n) v |= static_cast<unsigned int>(in[i + 2]);
			out.push_back(kB64[(v >> 18) & 0x3F]);
			out.push_back(kB64[(v >> 12) & 0x3F]);
			out.push_back((i + 1 < n) ? kB64[(v >> 6) & 0x3F] : '=');
			out.push_back((i + 2 < n) ? kB64[v & 0x3F] : '=');
		}

		return out;
	}

	static std::vector<unsigned char> base64Decode(const std::string& in, bool& ok) {
		ok = true;
		int dec[256];
		for (int i = 0; i < 256; ++i) dec[i] = -1;
		for (int i = 0; i < 64; ++i) dec[static_cast<unsigned char>(kB64[i])] = i;
		std::vector<unsigned char> out;
		int v = 0, bits = 0;
		for (char c : in) {
			if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
			if (c == '=') break;
			int d = dec[static_cast<unsigned char>(c)];
			if (d < 0) { ok = false; return out; }
			v = (v << 6) | d;
			bits += 6;
			if (bits >= 8) {
				bits -= 8;
				out.push_back(static_cast<unsigned char>((v >> bits) & 0xFF));
			}
		}

		return out;
	}

	static int builtin_base64(Executor& exec, const std::vector<std::string>& args) {
		bool decode = false;
		std::string file = "-";
		std::size_t wrap = 76;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-d" || a == "--decode") { decode = true; continue; }
			if (a == "-w" && i + 1 < args.size()) {
				unsigned long w = 0;
				if (parseUL(args[++i], w)) wrap = w;
				continue;
			}

			if (!a.empty() && a[0] == '-' && a != "-") continue;
			file = a;
		}

		FILE* fp = (file == "-") ? stdin : fopenNative(exec, file, "rb");
		if (!fp) {
			std::fprintf(stderr, "wbsh: base64: %s: %s\n",
				file.c_str(), std::strerror(errno));
			return 1;
		}

		std::vector<unsigned char> bytes;
		int c;
		while ((c = std::fgetc(fp)) != EOF) bytes.push_back(static_cast<unsigned char>(c));
		if (fp != stdin) std::fclose(fp);
		if (decode) {
			std::string s(bytes.begin(), bytes.end());
			bool ok;
			auto out = base64Decode(s, ok);
			if (!ok) { perr("base64", "invalid input"); return 1; }
			std::fwrite(out.data(), 1, out.size(), stdout);
		}
		else {
			std::string out = base64Encode(bytes);
			if (wrap > 0) {
				for (std::size_t i = 0; i < out.size(); i += wrap) {
					std::size_t take = (std::min)(wrap, out.size() - i);
					std::fwrite(out.data() + i, 1, take, stdout);
					std::fputc('\n', stdout);
				}
			}
			else {
				std::fwrite(out.data(), 1, out.size(), stdout);
				std::fputc('\n', stdout);
			}
		}

		std::fflush(stdout);
		return 0;
	}

	struct XxdOptions {
		bool plain = false;
		bool reverse = false;
		int cols = 16;
		std::string path;
	};

	static bool parseXxdArgs(const std::vector<std::string>& args, XxdOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-p" || a == "--plain")       o.plain = true;
			else if (a == "-r" || a == "--revert") o.reverse = true;
			else if (a == "-c" && i + 1 < args.size()) {
				parseInt(args[++i], o.cols);
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("xxd", "unknown option: " + a);
				return false;
			}
			else if (o.path.empty()) o.path = a;
		}

		return true;
	}

	static void xxdRevertLine(const std::string& line, bool plain,
	                          std::vector<unsigned char>& out) {
		std::string hex;
		if (plain) {
			hex = line;
		} else {
			StrScan in(line);
			(void)in.skipPast(":");  /* offset column; tolerate lines without one */
			if (!in.readUpTo("  ", hex)) hex = in.rest();
		}

		unsigned cur = 0;
		int half = 0;
		for (char c : hex) {
			unsigned v;
			if (c >= '0' && c <= '9') v = c - '0';
			else if (c >= 'a' && c <= 'f') v = 10 + c - 'a';
			else if (c >= 'A' && c <= 'F') v = 10 + c - 'A';
			else continue;
			cur = (cur << 4) | v;
			half++;
			if (half == 2) { out.push_back((unsigned char)cur); cur = 0; half = 0; }
		}
	}

	static int xxdEmitRevert(const std::vector<unsigned char>& bytes, bool plain) {
		std::vector<unsigned char> out;
		std::string line;
		for (unsigned char c : bytes) {
			if (c == '\n') { xxdRevertLine(line, plain, out); line.clear(); }
			else line.push_back((char)c);
		}

		if (!line.empty()) xxdRevertLine(line, plain, out);
		std::fwrite(out.data(), 1, out.size(), stdout);
		return 0;
	}

	static int xxdEmitPlain(const std::vector<unsigned char>& bytes, int cols) {
		int per_line = (cols > 0) ? cols * 2 : 60;
		int n = 0;
		for (unsigned char c : bytes) {
			std::printf("%02x", c);
			n += 2;
			if (n >= per_line) { std::printf("\n"); n = 0; }
		}

		if (n) std::printf("\n");
		return 0;
	}

	static int xxdEmitDefault(const std::vector<unsigned char>& bytes, int cols) {
		std::size_t addr = 0;
		while (addr < bytes.size()) {
			std::size_t end = (std::min)(bytes.size(), addr + (std::size_t)cols);
			std::printf("%08zx:", addr);
			for (std::size_t i = addr; i < addr + (std::size_t)cols; ++i) {
				if ((i - addr) % 2 == 0) std::printf(" ");
				if (i < end) std::printf("%02x", bytes[i]);
				else         std::printf("  ");
			}

			std::printf("  ");
			for (std::size_t i = addr; i < end; ++i) {
				unsigned char c = bytes[i];
				std::putchar((c >= 32 && c < 127) ? c : '.');
			}

			std::printf("\n");
			addr = end;
		}

		return 0;
	}

	static int builtin_xxd(Executor& exec, const std::vector<std::string>& args) {
		XxdOptions o;
		if (!parseXxdArgs(args, o)) return 1;

		std::vector<unsigned char> bytes;
		auto slurp = [&](FILE* f) {
			int c;
			while ((c = std::fgetc(f)) != EOF) bytes.push_back((unsigned char)c);
		};
		if (o.path.empty() || o.path == "-") slurp(stdin);
		else {
			FILE* f = fopenNative(exec, o.path, "rb");
			if (!f) {
				perr("xxd", o.path, std::error_code(errno, std::system_category()));
				return 1;
			}

			slurp(f);
			std::fclose(f);
		}

		if (o.reverse) return xxdEmitRevert(bytes, o.plain);
		if (o.plain)   return xxdEmitPlain(bytes, o.cols);
		return xxdEmitDefault(bytes, o.cols);
	}

	struct OdOptions {
		char fmt = 'o';
		char addr_fmt = 'o';
		std::string path;
	};

	static bool parseOdArgs(const std::vector<std::string>& args, OdOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-c") o.fmt = 'c';
			else if (a == "-x" || a == "-h") o.fmt = 'x';
			else if (a == "-d") o.fmt = 'd';
			else if (a == "-o") o.fmt = 'o';
			else if (a == "-A" && i + 1 < args.size()) {
				const std::string& w = args[++i];
				if (!w.empty()) o.addr_fmt = w[0];
			}
			else if (a == "-t" && i + 1 < args.size()) {
				const std::string& t = args[++i];
				if (!t.empty()) {
					char c = t[0];
					if (c == 'x' || c == 'd' || c == 'o' || c == 'c') o.fmt = c;
				}
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("od", "unknown option: " + a);
				return false;
			}
			else if (o.path.empty()) o.path = a;
		}

		return true;
	}

	static void odPrintAddr(std::size_t addr, char addr_fmt, bool with_newline) {
		const char* nl = with_newline ? "\n" : "";
		switch (addr_fmt) {
		case 'd': std::printf("%07zu%s", addr, nl); return;
		case 'x': std::printf("%07zx%s", addr, nl); return;
		case 'n': return;
		default:  std::printf("%07zo%s", addr, nl); return;
		}
	}

	static void odEmitRow(const std::vector<unsigned char>& bytes,
	                      std::size_t addr, std::size_t end, char fmt) {
		if (fmt == 'c') {
			for (std::size_t i = addr; i < end; ++i) {
				unsigned char b = bytes[i];
				const char* esc = nullptr;
				switch (b) {
				case '\0': esc = "\\0"; break;
				case '\a': esc = "\\a"; break;
				case '\b': esc = "\\b"; break;
				case '\t': esc = "\\t"; break;
				case '\n': esc = "\\n"; break;
				case '\v': esc = "\\v"; break;
				case '\f': esc = "\\f"; break;
				case '\r': esc = "\\r"; break;
				}

				if (esc) std::printf("  %s", esc);
				else if (b >= 32 && b < 127) std::printf("   %c", b);
				else std::printf(" %03o", b);
			}

			return;
		}

		const char* spec = (fmt == 'x') ? " %02x" : (fmt == 'd') ? " %3u" : " %03o";
		for (std::size_t i = addr; i < end; ++i) std::printf(spec, bytes[i]);
	}

	static int builtin_od(Executor& exec, const std::vector<std::string>& args) {
		OdOptions o;
		if (!parseOdArgs(args, o)) return 1;

		std::vector<unsigned char> bytes;
		auto slurp = [&](FILE* f) {
			int c; while ((c = std::fgetc(f)) != EOF) bytes.push_back((unsigned char)c);
		};
		if (o.path.empty() || o.path == "-") slurp(stdin);
		else {
			FILE* f = fopenNative(exec, o.path, "rb");
			if (!f) {
				perr("od", o.path, std::error_code(errno, std::system_category()));
				return 1;
			}

			slurp(f);
			std::fclose(f);
		}

		constexpr std::size_t per_row = 16;
		std::size_t addr = 0;
		while (addr < bytes.size()) {
			odPrintAddr(addr, o.addr_fmt, /*with_newline=*/false);
			std::size_t end = (std::min)(bytes.size(), addr + per_row);
			odEmitRow(bytes, addr, end, o.fmt);
			std::printf("\n");
			addr = end;
		}

		odPrintAddr(bytes.size(), o.addr_fmt, /*with_newline=*/true);
		return 0;
	}

	void registerEncodingBuiltins(Executor& exec) {
		exec.registerBuiltin("xxd",    builtin_xxd);
		exec.registerBuiltin("od",     builtin_od);
		exec.registerBuiltin("base64", builtin_base64);
	}

}  // namespace wbsh
