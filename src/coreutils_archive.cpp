/**
 * @file coreutils_archive.cpp
 * @brief Archive / compression coreutils: tar (ustar, uncompressed),
 *        gzip, gunzip, zcat, zip, unzip (zip writes stored entries
 *        only; both directions stay compatible with other tools).
 */

#include "coreutils_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "executor.h"
#include "inflate.h"
#include "numparse.h"

namespace wbsh {

	namespace fs = std::filesystem;

	struct TarHeader {
		char name[100];
		char mode[8];
		char uid[8];
		char gid[8];
		char size[12];
		char mtime[12];
		char chksum[8];
		char typeflag;
		char linkname[100];
		char magic[6];     // "ustar\0"
		char version[2];   // "00"
		char uname[32];
		char gname[32];
		char devmajor[8];
		char devminor[8];
		char prefix[155];
		char pad[12];
	};
	static_assert(sizeof(TarHeader) == 512, "tar header must be 512 bytes");

	static void tarOctal(char* dst, std::size_t width, std::uintmax_t v) {
		std::string s;
		while (v > 0) { s.insert(s.begin(), char('0' + (v & 7))); v >>= 3; }
		if (s.empty()) s = "0";
		while (s.size() < width - 1) s.insert(s.begin(), '0');
		std::memset(dst, 0, width);
		std::memcpy(dst, s.data(), (std::min)(s.size(), width - 1));
	}

	std::uintmax_t tarParseOctal(const char* p, std::size_t n) {
		std::uintmax_t v = 0;
		for (std::size_t i = 0; i < n && p[i] && p[i] != ' '; ++i) {
			if (p[i] < '0' || p[i] > '7') break;
			v = (v << 3) | (p[i] - '0');
		}

		return v;
	}

	static void tarFillChecksum(TarHeader* h) {
		std::memset(h->chksum, ' ', sizeof(h->chksum));
		std::uintmax_t s = 0;
		const unsigned char* p = reinterpret_cast<const unsigned char*>(h);
		for (std::size_t i = 0; i < sizeof(*h); ++i) s += p[i];
		tarOctal(h->chksum, 7, s);
		h->chksum[6] = '\0';
		h->chksum[7] = ' ';
	}

	static std::time_t tarFileMtime(const fs::path& src) {
		std::error_code ec;
		auto t = fs::last_write_time(src, ec);
		if (ec) return 0;
		return std::chrono::system_clock::to_time_t(
			std::chrono::system_clock::now()
			+ std::chrono::duration_cast<std::chrono::system_clock::duration>(
				t - fs::file_time_type::clock::now()));
	}

	static bool tarWriteFileData(FILE* out, const fs::path& src, std::uintmax_t size) {
		FILE* in = openUtf8(pathToUtf8(src), "rb");
		if (!in) return false;
		char buf[512];
		std::uintmax_t left = size;
		while (left > 0) {
			std::size_t want = (std::min)(static_cast<std::uintmax_t>(512), left);
			std::size_t got = std::fread(buf, 1, want, in);
			if (got == 0) break;
			std::fwrite(buf, 1, got, out);
			if (got < 512) {
				char zero[512] = {};
				std::fwrite(zero, 1, 512 - got, out);
			}

			left -= got;
		}

		std::fclose(in);
		return true;
	}

	static bool tarWriteEntry(FILE* out, const fs::path& src, const std::string& rel,
		bool verbose) {
		std::error_code ec;
		fs::file_status st = fs::symlink_status(src, ec);
		if (ec) return false;

		TarHeader h{};
		std::string name = rel;
		std::replace(name.begin(), name.end(), '\\', '/');
		if (fs::is_directory(st)) {
			if (!name.empty() && name.back() != '/') name.push_back('/');
			h.typeflag = '5';
		}
		else {
			h.typeflag = '0';
		}

		if (name.size() >= sizeof(h.name)) {
			std::fprintf(stderr, "wbsh: tar: name too long: %s\n", name.c_str());
			return false;
		}

		std::strncpy(h.name, name.c_str(), sizeof(h.name) - 1);
		tarOctal(h.mode, 8, 0644);
		tarOctal(h.uid, 8, 0);
		tarOctal(h.gid, 8, 0);
		std::uintmax_t size = (h.typeflag == '5') ? 0 : fs::file_size(src, ec);
		if (ec) size = 0;
		tarOctal(h.size, 12, size);
		tarOctal(h.mtime, 12, static_cast<std::uintmax_t>(tarFileMtime(src)));
		std::memcpy(h.magic, "ustar\0", 6);
		std::memcpy(h.version, "00", 2);
		tarFillChecksum(&h);
		std::fwrite(&h, 1, sizeof(h), out);
		if (verbose) std::fprintf(stderr, "%s\n", name.c_str());

		if (h.typeflag != '5' && size > 0) {
			return tarWriteFileData(out, src, size);
		}

		return true;
	}

	static int tarCreate(Executor& exec, const std::string& archive,
		const std::vector<std::string>& items, bool verbose) {
		FILE* out = fopenNative(exec, archive, "wb");
		if (!out) { perr("tar", archive + ": " + std::strerror(errno)); return 1; }
		int rc = 0;
		for (const auto& item : items) {
			fs::path nat(toNative(exec, item));
			std::error_code ec;
			if (!fs::exists(nat, ec)) {
				perr("tar", item + ": not found");
				rc = 1;
				continue;
			}

			if (fs::is_directory(nat, ec)) {
				tarWriteEntry(out, nat, item, verbose);
				fs::recursive_directory_iterator it(nat,
					fs::directory_options::skip_permission_denied, ec);
				if (ec) continue;
				for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
					if (ec) break;
					std::error_code rec;
					std::string rel = item + "/" + pathToUtf8(fs::relative(cur->path(), nat, rec));
					tarWriteEntry(out, cur->path(), rel, verbose);
				}
			}
			else {
				tarWriteEntry(out, nat, item, verbose);
			}
		}

		char zero[1024] = {};
		std::fwrite(zero, 1, 1024, out);
		std::fclose(out);
		return rc;
	}

	static void tarSkipPadded(FILE* in, std::uintmax_t size) {
		std::uintmax_t skip = (size + 511) & ~static_cast<std::uintmax_t>(511);
		std::fseek(in, static_cast<long>(skip), SEEK_CUR);
	}

	static void tarCopyFileData(FILE* in, FILE* out, std::uintmax_t size) {
		char buf[512];
		std::uintmax_t left = size;
		while (left > 0) {
			std::size_t r = std::fread(buf, 1, 512, in);
			if (r == 0) break;
			std::size_t writeN = (left >= 512) ? 512 : static_cast<std::size_t>(left);
			std::fwrite(buf, 1, writeN, out);
			left -= writeN;
		}
	}

	static bool tarHeaderIsAllZero(const TarHeader& h) {
		const unsigned char* p = reinterpret_cast<const unsigned char*>(&h);
		for (std::size_t k = 0; k < sizeof(h); ++k) {
			if (p[k]) return false;
		}

		return true;
	}

	static int tarExtract(Executor& exec, const std::string& archive,
	                      bool verbose, bool list_only) {
		FILE* in = fopenNative(exec, archive, "rb");
		if (!in) { perr("tar", archive + ": " + std::strerror(errno)); return 1; }

		int rc = 0;
		while (true) {
			TarHeader h{};
			std::size_t got = std::fread(&h, 1, sizeof(h), in);
			if (got != sizeof(h)) break;
			if (tarHeaderIsAllZero(h)) break;

			std::string name(h.name, ::strnlen(h.name, sizeof(h.name)));
			std::uintmax_t size = tarParseOctal(h.size, sizeof(h.size));
			bool is_dir = (h.typeflag == '5')
				|| (!name.empty() && name.back() == '/');
			if (verbose || list_only) std::printf("%s\n", name.c_str());
			if (list_only) {
				tarSkipPadded(in, size);
				continue;
			}

			if (is_dir) {
				std::error_code ec;
				fs::create_directories(toNative(exec, name), ec);
				tarSkipPadded(in, size);
				continue;
			}

			{
				fs::path p(toNative(exec, name));
				std::error_code ec;
				fs::create_directories(p.parent_path(), ec);
			}

			FILE* out = fopenNative(exec, name, "wb");
			if (!out) {
				perr("tar", name + ": " + std::strerror(errno));
				rc = 1;
				tarSkipPadded(in, size);
				continue;
			}

			tarCopyFileData(in, out, size);
			std::fclose(out);
		}

		std::fclose(in);
		return rc;
	}

	static int builtin_tar(Executor& exec, const std::vector<std::string>& args) {
		char mode = 0;
		bool verbose = false;
		std::string archive;
		std::vector<std::string> items;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (!a.empty() && a[0] == '-' && a.size() > 1) {
				for (std::size_t k = 1; k < a.size(); ++k) {
					char c = a[k];
					if (c == 'c' || c == 'x' || c == 't') mode = c;
					else if (c == 'v') verbose = true;
					else if (c == 'f') {
						if (k + 1 < a.size()) archive = a.substr(k + 1);
						else if (i + 1 < args.size()) archive = args[++i];
						k = a.size();
					}
				}
				continue;
			}

			items.push_back(a);
		}

		if (mode == 0) { perr("tar", "specify -c, -x, or -t"); return 2; }
		if (archive.empty()) { perr("tar", "missing -f ARCHIVE"); return 2; }
		switch (mode) {
		case 'c': return tarCreate(exec, archive, items, verbose);
		case 'x': return tarExtract(exec, archive, verbose, false);
		case 't': return tarExtract(exec, archive, false, true);
		}

		return 2;
	}

	// CRC-32/IEEE 802.3 (poly 0xEDB88320). Required for gzip footer.
	std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* buf, std::size_t n) {
		static std::uint32_t table[256];
		static bool inited = false;
		if (!inited) {
			for (std::uint32_t i = 0; i < 256; ++i) {
				std::uint32_t c = i;
				for (int k = 0; k < 8; ++k)
					c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
				table[i] = c;
			}

			inited = true;
		}

		crc ^= 0xFFFFFFFFu;
		for (std::size_t i = 0; i < n; ++i)
			crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	static std::vector<std::uint8_t> readAllBytesFromFile(FILE* f) {
		std::vector<std::uint8_t> out;
		std::uint8_t buf[4096];
		while (true) {
			std::size_t n = std::fread(buf, 1, sizeof(buf), f);
			if (n == 0) break;
			out.insert(out.end(), buf, buf + n);
		}

		return out;
	}

	static bool gzipParseHeader(const std::vector<std::uint8_t>& bytes,
		std::size_t& deflate_start,
		std::size_t& deflate_end) {
		if (bytes.size() < 18) return false;
		if (bytes[0] != 0x1F || bytes[1] != 0x8B) return false;
		if (bytes[2] != 8) return false;   // CM = deflate
		std::uint8_t flg = bytes[3];
		std::size_t i = 10;   // skip MTIME, XFL, OS
		if (flg & 0x04) {     // FEXTRA
			if (i + 2 > bytes.size()) return false;
			std::size_t xlen = bytes[i] | (bytes[i + 1] << 8);
			i += 2 + xlen;
		}

		if (flg & 0x08) {     // FNAME
			while (i < bytes.size() && bytes[i] != 0) ++i;
			if (i < bytes.size()) ++i;
		}

		if (flg & 0x10) {     // FCOMMENT
			while (i < bytes.size() && bytes[i] != 0) ++i;
			if (i < bytes.size()) ++i;
		}

		if (flg & 0x02) i += 2;   // FHCRC
		if (i + 8 > bytes.size()) return false;
		deflate_start = i;
		deflate_end = bytes.size() - 8;
		return true;
	}

	static void appendGzipHeader(std::vector<std::uint8_t>& out) {
		out.push_back(0x1F);
		out.push_back(0x8B);
		out.push_back(0x08);   // CM = deflate
		out.push_back(0x00);   // FLG = 0
		for (int k = 0; k < 4; ++k) out.push_back(0x00);   // MTIME
		out.push_back(0x00);   // XFL
		out.push_back(0xFF);   // OS = unknown
	}

	static void appendDeflateStoredBlocks(const std::vector<std::uint8_t>& data,
		std::vector<std::uint8_t>& out) {
		std::size_t i = 0;
		std::size_t n = data.size();
		if (n == 0) {
			out.push_back(0x01);
			out.push_back(0x00); out.push_back(0x00);
			out.push_back(0xFF); out.push_back(0xFF);
		}

		while (i < n) {
			std::size_t take = n - i;
			if (take > 65535) take = 65535;
			bool last = (i + take >= n);
			out.push_back(last ? 0x01 : 0x00);
			out.push_back((std::uint8_t)(take & 0xFF));
			out.push_back((std::uint8_t)((take >> 8) & 0xFF));
			std::uint16_t nlen = (std::uint16_t)~take;
			out.push_back((std::uint8_t)(nlen & 0xFF));
			out.push_back((std::uint8_t)((nlen >> 8) & 0xFF));
			out.insert(out.end(), data.begin() + i, data.begin() + i + take);
			i += take;
		}
	}

	static void appendGzipFooter(const std::vector<std::uint8_t>& data,
		std::vector<std::uint8_t>& out) {
		std::uint32_t crc = crc32Update(0, data.data(), data.size());
		std::uint32_t isize = (std::uint32_t)(data.size() & 0xFFFFFFFFu);
		for (int k = 0; k < 4; ++k) out.push_back((std::uint8_t)((crc >> (8 * k)) & 0xFF));
		for (int k = 0; k < 4; ++k) out.push_back((std::uint8_t)((isize >> (8 * k)) & 0xFF));
	}

	static void gzipEncodeStored(const std::vector<std::uint8_t>& data,
		std::vector<std::uint8_t>& out) {
		appendGzipHeader(out);
		appendDeflateStoredBlocks(data, out);
		appendGzipFooter(data, out);
	}

	struct GzipOptions {
		bool decompress = false;
		bool to_stdout = false;
		bool keep = false;
		std::vector<std::string> files;
	};

	static GzipOptions parseGzipArgs(const std::vector<std::string>& args) {
		GzipOptions o;
		for (const auto& a : args) {
			if (a == "-d" || a == "--decompress") o.decompress = true;
			else if (a == "-c" || a == "--stdout") o.to_stdout = true;
			else if (a == "-k" || a == "--keep") o.keep = true;
			else if (a == "-f" || a == "--force") { /* accepted, no-op */ }
			else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore other flags */ }
			else o.files.push_back(a);
		}

		return o;
	}

	static bool gzipLoadInput(Executor& exec, const std::string& tool, const std::string& fname,
	                          std::vector<std::uint8_t>& input) {
		if (fname == "-" || fname.empty()) {
			input = readAllBytesFromFile(stdin);
			return true;
		}

		FILE* f = fopenNative(exec, fname, "rb");
		if (!f) {
			perr(tool, fname, std::error_code(errno, std::system_category()));
			return false;
		}

		input = readAllBytesFromFile(f);
		std::fclose(f);
		return true;
	}

	static std::string gzipDeriveOutputPath(const std::string& fname, bool decompress) {
		if (decompress) {
			if (fname.size() > 3 && fname.substr(fname.size() - 3) == ".gz") {
				return fname.substr(0, fname.size() - 3);
			}

			return fname + ".out";
		}

		return fname + ".gz";
	}

	static int gzipTransform(const GzipOptions& opt, const std::string& fname,
	                         const std::vector<std::uint8_t>& input,
	                         std::vector<std::uint8_t>& output) {
		if (!opt.decompress) {
			gzipEncodeStored(input, output);
			return 0;
		}

		std::size_t s, e;
		if (!gzipParseHeader(input, s, e)) {
			std::fprintf(stderr, "wbsh: gunzip: not in gzip format: %s\n", fname.c_str());
			return 1;
		}

		if (!inflateRaw(input.data() + s, e - s, output)) {
			std::fprintf(stderr, "wbsh: gunzip: invalid compressed data: %s\n", fname.c_str());
			return 1;
		}

		return 0;
	}

	static int gzipProcessOne(Executor& exec, const GzipOptions& opt, const std::string& fname) {
		const char* tool = opt.decompress ? "gunzip" : "gzip";
		std::vector<std::uint8_t> input;
		if (!gzipLoadInput(exec, tool, fname, input)) return 1;
		std::vector<std::uint8_t> output;
		if (int rc = gzipTransform(opt, fname, input, output); rc != 0) return rc;
		FILE* of = stdout;
		std::string out_path;
		if (!opt.to_stdout && !fname.empty() && fname != "-") {
			out_path = gzipDeriveOutputPath(fname, opt.decompress);
			of = fopenNative(exec, out_path, "wb");
			if (!of) {
				perr(tool, out_path, std::error_code(errno, std::system_category()));
				return 1;
			}
		}

		std::fwrite(output.data(), 1, output.size(), of);
		if (of != stdout) std::fclose(of);
		if (!opt.to_stdout && !opt.keep && !fname.empty() && fname != "-") {
			std::error_code ec;
			std::filesystem::remove(toNative(exec, fname), ec);
		}

		return 0;
	}

	static int builtin_gzip(Executor& exec, const std::vector<std::string>& args) {
		GzipOptions opt = parseGzipArgs(args);
		if (opt.files.empty()) {
			opt.to_stdout = true;
			return gzipProcessOne(exec, opt, "");
		}

		int rc = 0;
		for (const auto& f : opt.files) {
			int r = gzipProcessOne(exec, opt, f);
			if (r) rc = r;
		}

		return rc;
	}

	static int builtin_gunzip(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> a = { "-d" };
		a.insert(a.end(), args.begin(), args.end());
		return builtin_gzip(exec, a);
	}

	static int builtin_zcat(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> a = { "-d", "-c" };
		a.insert(a.end(), args.begin(), args.end());
		return builtin_gzip(exec, a);
	}

	std::uint16_t zipR16(const std::uint8_t* p) {
		return (std::uint16_t)(p[0] | (p[1] << 8));
	}

	std::uint32_t zipR32(const std::uint8_t* p) {
		return (std::uint32_t)p[0]
			| ((std::uint32_t)p[1] << 8)
			| ((std::uint32_t)p[2] << 16)
			| ((std::uint32_t)p[3] << 24);
	}

	static void zipW16(std::vector<std::uint8_t>& out, std::uint16_t v) {
		out.push_back((std::uint8_t)(v & 0xFF));
		out.push_back((std::uint8_t)((v >> 8) & 0xFF));
	}

	static void zipW32(std::vector<std::uint8_t>& out, std::uint32_t v) {
		out.push_back((std::uint8_t)(v & 0xFF));
		out.push_back((std::uint8_t)((v >> 8) & 0xFF));
		out.push_back((std::uint8_t)((v >> 16) & 0xFF));
		out.push_back((std::uint8_t)((v >> 24) & 0xFF));
	}

	static bool zipFindEOCD(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
		if (bytes.size() < 22) return false;
		std::size_t max_back = bytes.size() < 65557 ? bytes.size() : 65557;
		for (std::size_t i = bytes.size() - 22; i + 22 >= bytes.size() - max_back; --i) {
			if (zipR32(bytes.data() + i) == 0x06054B50u) { pos = i; return true; }
			if (i == 0) break;
		}

		return false;
	}

	namespace unzip_internal {
		struct UnzipOptions {
			bool list_only = false;
			bool to_stdout = false;
			std::string archive;
			std::vector<std::string> select;
			std::string outdir;
		};

		struct CentralEntry {
			std::uint16_t method;
			std::uint32_t csize;
			std::uint32_t usize;
			std::uint32_t lfh_off;
			std::string name;
		};
	}  // namespace unzip_internal

	static unzip_internal::UnzipOptions parseUnzipArgs(const std::vector<std::string>& args) {
		unzip_internal::UnzipOptions o;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if      (a == "-l") o.list_only = true;
			else if (a == "-p") o.to_stdout = true;
			else if (a == "-o") { /* overwrite (default) */ }
			else if (a == "-n") { /* never overwrite — not implemented */ }
			else if (a == "-d" && i + 1 < args.size()) o.outdir = args[++i];
			else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore */ }
			else if (o.archive.empty()) o.archive = a;
			else o.select.push_back(a);
		}

		return o;
	}

	static bool readZipCentralEntry(const std::vector<std::uint8_t>& bytes, std::size_t* p,
	                                unzip_internal::CentralEntry& e) {
		if (*p + 46 > bytes.size()) return false;
		if (zipR32(bytes.data() + *p) != 0x02014B50u) return false;
		e.method  = zipR16(bytes.data() + *p + 10);
		e.csize   = zipR32(bytes.data() + *p + 20);
		e.usize   = zipR32(bytes.data() + *p + 24);
		const std::uint16_t nlen = zipR16(bytes.data() + *p + 28);
		const std::uint16_t xlen = zipR16(bytes.data() + *p + 30);
		const std::uint16_t clen = zipR16(bytes.data() + *p + 32);
		e.lfh_off = zipR32(bytes.data() + *p + 42);
		e.name.assign(reinterpret_cast<const char*>(bytes.data() + *p + 46), nlen);
		*p += 46 + nlen + xlen + clen;
		return true;
	}

	static bool decompressZipEntry(const std::vector<std::uint8_t>& bytes,
	                               const unzip_internal::CentralEntry& e,
	                               std::vector<std::uint8_t>& out_data) {
		if (e.lfh_off + 30 > bytes.size()) return false;
		if (zipR32(bytes.data() + e.lfh_off) != 0x04034B50u) return false;
		const std::uint16_t l_nlen = zipR16(bytes.data() + e.lfh_off + 26);
		const std::uint16_t l_xlen = zipR16(bytes.data() + e.lfh_off + 28);
		const std::size_t data_off = e.lfh_off + 30 + l_nlen + l_xlen;
		if (data_off + e.csize > bytes.size()) return false;

		if (e.method == 0) {
			out_data.assign(bytes.begin() + data_off,
			                bytes.begin() + data_off + e.csize);
			return true;
		}

		if (e.method == 8) {
			return inflateRaw(bytes.data() + data_off, e.csize, out_data);
		}

		return false;
	}

	static void writeZipEntryToDisk(Executor& exec, const std::string& outdir,
	                                const std::string& name,
	                                const std::vector<std::uint8_t>& data) {
		const std::string out_path = outdir.empty() ? name : (outdir + "/" + name);
		if (!name.empty() && name.back() == '/') {
			std::error_code ec;
			std::filesystem::create_directories(toNative(exec, out_path), ec);
			return;
		}

		std::filesystem::path pp = toNative(exec, out_path);
		std::error_code ec;
		if (pp.has_parent_path()) {
			std::filesystem::create_directories(pp.parent_path(), ec);
		}

		FILE* of = openUtf8(pathToUtf8(pp), "wb");
		if (!of) {
			perr("unzip", out_path,
				std::error_code(errno, std::system_category()));
			return;
		}

		std::fwrite(data.data(), 1, data.size(), of);
		std::fclose(of);
	}

	static bool entryIsSelected(const std::vector<std::string>& select,
	                            const std::string& name) {
		if (select.empty()) return true;
		for (const auto& s : select) if (s == name) return true;
		return false;
	}

	static bool unzipLoadArchive(Executor& exec, const std::string& path,
	                             std::vector<std::uint8_t>& out) {
		FILE* f = fopenNative(exec, path, "rb");
		if (!f) {
			perr("unzip", path, std::error_code(errno, std::system_category()));
			return false;
		}

		out = readAllBytesFromFile(f);
		std::fclose(f);
		return true;
	}

	static void unzipHandleEntry(Executor& exec, const unzip_internal::UnzipOptions& o,
	                             const std::vector<std::uint8_t>& bytes,
	                             const unzip_internal::CentralEntry& e,
	                             std::size_t& total_bytes) {
		if (o.list_only) {
			std::printf("%9u  ----------- ------  %s\n",
				static_cast<unsigned>(e.usize), e.name.c_str());
			total_bytes += e.usize;
			return;
		}

		std::vector<std::uint8_t> data;
		if (!decompressZipEntry(bytes, e, data)) {
			std::fprintf(stderr,
				"wbsh: unzip: inflate / unsupported-method on %s\n", e.name.c_str());
			return;
		}

		if (o.to_stdout) {
			std::fwrite(data.data(), 1, data.size(), stdout);
			return;
		}

		writeZipEntryToDisk(exec, o.outdir, e.name, data);
		std::printf("  inflating: %s\n", e.name.c_str());
	}

	static int builtin_unzip(Executor& exec, const std::vector<std::string>& args) {
		const unzip_internal::UnzipOptions o = parseUnzipArgs(args);
		if (o.archive.empty()) {
			perr("unzip", "missing archive name");
			return 1;
		}

		std::vector<std::uint8_t> bytes;
		if (!unzipLoadArchive(exec, o.archive, bytes)) return 1;

		std::size_t eocd = 0;
		if (!zipFindEOCD(bytes, eocd)) {
			perr("unzip", "not a zip archive: " + o.archive);
			return 1;
		}

		const std::uint16_t total = zipR16(bytes.data() + eocd + 10);
		std::size_t p = zipR32(bytes.data() + eocd + 16);

		if (o.list_only) {
			std::printf("Archive:  %s\n", o.archive.c_str());
			std::printf("  Length      Date    Time    Name\n");
			std::printf("---------  ---------- -----   ----\n");
		}

		std::size_t total_bytes = 0;
		for (std::size_t k = 0; k < total; ++k) {
			unzip_internal::CentralEntry e;
			if (!readZipCentralEntry(bytes, &p, e)) break;
			if (!entryIsSelected(o.select, e.name)) continue;
			unzipHandleEntry(exec, o, bytes, e, total_bytes);
		}

		if (o.list_only) {
			std::printf("---------                     -------\n");
			std::printf("%9zu                     %u files\n",
				total_bytes, static_cast<unsigned>(total));
		}

		return 0;
	}

	namespace zip_internal {
		struct ZipCdEntry {
			std::string name;
			std::uint32_t crc;
			std::uint32_t size;
			std::uint32_t lfh_off;
		};
	}  // namespace zip_internal

	static std::vector<std::string> gatherZipInputs(Executor& exec,
	                                                const std::vector<std::string>& inputs,
	                                                bool recurse) {
		namespace fs = std::filesystem;
		std::vector<std::string> paths;
		for (const auto& in : inputs) {
			const fs::path win = toNative(exec, in);
			std::error_code ec;
			if (fs::is_directory(win, ec) && recurse) {
				for (auto it = fs::recursive_directory_iterator(win, ec);
				     it != fs::recursive_directory_iterator(); it.increment(ec))
				{
					if (ec) break;
					if (it->is_regular_file(ec)) {
						std::string rel = pathToUtf8(fs::relative(it->path(),
							fs::current_path(ec)));
						std::replace(rel.begin(), rel.end(), '\\', '/');
						paths.push_back(std::move(rel));
					}
				}
			} else if (fs::is_regular_file(win, ec)) {
				paths.push_back(in);
			}
		}

		return paths;
	}

	static zip_internal::ZipCdEntry writeZipLocalEntry(Executor& exec,
	                                                   const std::string& path,
	                                                   std::vector<std::uint8_t>& out) {
		zip_internal::ZipCdEntry meta{ path, 0, 0, 0 };

		FILE* f = fopenNative(exec, path, "rb");
		if (!f) {
			perr("zip", path, std::error_code(errno, std::system_category()));
			meta.name.clear();
			return meta;
		}

		const auto data = readAllBytesFromFile(f);
		std::fclose(f);

		const std::uint32_t crc = crc32Update(0, data.data(), data.size());
		const std::uint32_t lfh_off = static_cast<std::uint32_t>(out.size());

		zipW32(out, 0x04034B50u);
		zipW16(out, 20);                                // version needed
		zipW16(out, 0);                                 // flags
		zipW16(out, 0);                                 // method = stored
		zipW16(out, 0);                                 // mod time
		zipW16(out, 0);                                 // mod date
		zipW32(out, crc);
		zipW32(out, static_cast<std::uint32_t>(data.size()));
		zipW32(out, static_cast<std::uint32_t>(data.size()));
		zipW16(out, static_cast<std::uint16_t>(path.size()));
		zipW16(out, 0);                                 // extra
		out.insert(out.end(), path.begin(), path.end());
		out.insert(out.end(), data.begin(), data.end());

		meta.crc = crc;
		meta.size = static_cast<std::uint32_t>(data.size());
		meta.lfh_off = lfh_off;
		return meta;
	}

	static void writeZipCentralDir(std::vector<std::uint8_t>& out,
	                               const std::vector<zip_internal::ZipCdEntry>& cd) {
		for (const auto& e : cd) {
			zipW32(out, 0x02014B50u);
			zipW16(out, 20);                            // version made
			zipW16(out, 20);                            // version needed
			zipW16(out, 0);                             // flags
			zipW16(out, 0);                             // method
			zipW16(out, 0);                             // mod time
			zipW16(out, 0);                             // mod date
			zipW32(out, e.crc);
			zipW32(out, e.size);                        // comp size
			zipW32(out, e.size);                        // uncomp size
			zipW16(out, static_cast<std::uint16_t>(e.name.size()));
			zipW16(out, 0);                             // extra
			zipW16(out, 0);                             // comment len
			zipW16(out, 0);                             // disk
			zipW16(out, 0);                             // int attr
			zipW32(out, 0);                             // ext attr
			zipW32(out, e.lfh_off);
			out.insert(out.end(), e.name.begin(), e.name.end());
		}
	}

	static void writeZipEocd(std::vector<std::uint8_t>& out,
	                         std::uint32_t cd_off,
	                         std::uint32_t cd_size,
	                         std::uint16_t entry_count) {
		zipW32(out, 0x06054B50u);
		zipW16(out, 0);
		zipW16(out, 0);
		zipW16(out, entry_count);
		zipW16(out, entry_count);
		zipW32(out, cd_size);
		zipW32(out, cd_off);
		zipW16(out, 0);
	}

	static int builtin_zip(Executor& exec, const std::vector<std::string>& args) {
		bool recurse = false;
		std::string archive;
		std::vector<std::string> inputs;
		for (const auto& a : args) {
			if (a == "-r" || a == "--recurse-paths") recurse = true;
			else if (!a.empty() && a[0] == '-' && a != "-") { /* ignore */ }
			else if (archive.empty()) archive = a;
			else inputs.push_back(a);
		}

		if (archive.empty()) { perr("zip", "missing archive name"); return 1; }
		if (inputs.empty())  { perr("zip", "no input files");      return 1; }

		const std::vector<std::string> paths = gatherZipInputs(exec, inputs, recurse);

		std::vector<std::uint8_t> out;
		std::vector<zip_internal::ZipCdEntry> cd;
		for (const auto& path : paths) {
			zip_internal::ZipCdEntry e = writeZipLocalEntry(exec, path, out);
			if (e.name.empty()) continue;
			std::printf("  adding: %s (stored)\n", path.c_str());
			cd.push_back(std::move(e));
		}

		const std::uint32_t cd_off = static_cast<std::uint32_t>(out.size());
		writeZipCentralDir(out, cd);
		const std::uint32_t cd_size = static_cast<std::uint32_t>(out.size() - cd_off);
		writeZipEocd(out, cd_off, cd_size,
		             static_cast<std::uint16_t>(cd.size()));

		FILE* of = fopenNative(exec, archive, "wb");
		if (!of) {
			perr("zip", archive, std::error_code(errno, std::system_category()));
			return 1;
		}

		std::fwrite(out.data(), 1, out.size(), of);
		std::fclose(of);
		return 0;
	}

	void registerArchiveBuiltins(Executor& exec) {
		exec.registerBuiltin("gzip",   builtin_gzip);
		exec.registerBuiltin("gunzip", builtin_gunzip);
		exec.registerBuiltin("zcat",   builtin_zcat);
		exec.registerBuiltin("zip",    builtin_zip);
		exec.registerBuiltin("unzip",  builtin_unzip);
		exec.registerBuiltin("tar",    builtin_tar);
	}

}  // namespace wbsh
