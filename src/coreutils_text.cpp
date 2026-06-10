/**
 * @file coreutils_text.cpp
 * @brief Text-processing coreutils: sort, uniq, tr, cut, tee, paste,
 *        tac, rev, nl, grep, find, sed, fold, column, expand,
 *        unexpand, comm, xargs (awk lives in awk.cpp).
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include "awk.h"
#include "executor.h"
#include "numparse.h"
#include "regexutil.h"

namespace wbsh {

	namespace fs = std::filesystem;

	struct SortOptions {
		bool reverse = false, numeric = false, unique = false, fold = false;
		std::vector<std::string> files;
	};

	static int parseSortArgs(const std::vector<std::string>& args, SortOptions& o) {
		for (const auto& a : args) {
			if (a == "--") continue;
			if (a == "-r" || a == "--reverse") o.reverse = true;
			else if (a == "-n" || a == "--numeric-sort") o.numeric = true;
			else if (a == "-u" || a == "--unique") o.unique = true;
			else if (a == "-f" || a == "--ignore-case") o.fold = true;
			else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					switch (a[k]) {
					case 'r': o.reverse = true; break;
					case 'n': o.numeric = true; break;
					case 'u': o.unique = true; break;
					case 'f': o.fold = true; break;
					default:
						std::fprintf(stderr, "wbsh: sort: unknown -%c\n", a[k]);
						return 2;
					}
				}
			}
			else o.files.push_back(a);
		}

		if (o.files.empty()) o.files.push_back("-");
		return 0;
	}

	static std::string asciiLower(std::string s) {
		for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
		return s;
	}

	static bool equalFold(const std::string& a, const std::string& b) {
		if (a.size() != b.size()) return false;
		for (std::size_t i = 0; i < a.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(a[i]))
			    != std::tolower(static_cast<unsigned char>(b[i]))) return false;
		}

		return true;
	}

	static void sortLinesNumeric(std::vector<std::string>& lines) {
		std::vector<std::pair<double, std::string>> keyed;
		keyed.reserve(lines.size());
		for (auto& l : lines) {
			double d = 0;
			parseDouble(l, d);
			keyed.emplace_back(d, std::move(l));
		}

		std::sort(keyed.begin(), keyed.end());
		for (std::size_t i = 0; i < keyed.size(); ++i) {
			lines[i] = std::move(keyed[i].second);
		}
	}

	static void sortLinesFolded(std::vector<std::string>& lines) {
		std::vector<std::pair<std::string, std::string>> keyed;
		keyed.reserve(lines.size());
		for (auto& l : lines) {
			std::string key = asciiLower(l);
			keyed.emplace_back(std::move(key), std::move(l));
		}

		std::sort(keyed.begin(), keyed.end());
		for (std::size_t i = 0; i < keyed.size(); ++i) {
			lines[i] = std::move(keyed[i].second);
		}
	}

	static int builtin_sort(Executor& exec, const std::vector<std::string>& args) {
		SortOptions opt;
		if (int rc = parseSortArgs(args, opt); rc != 0) return rc;

		std::vector<std::string> lines;
		for (const auto& f : opt.files) {
			if (!readAllLines(exec, f, lines)) {
				perr("sort", f + ": " + std::strerror(errno));
				return 2;
			}
		}

		if (opt.numeric)   sortLinesNumeric(lines);
		else if (opt.fold) sortLinesFolded(lines);
		else               std::sort(lines.begin(), lines.end());
		if (opt.reverse) std::reverse(lines.begin(), lines.end());
		if (opt.unique) {
			auto eq = [&](const std::string& a, const std::string& b) {
				return opt.fold ? equalFold(a, b) : a == b;
			};
			lines.erase(std::unique(lines.begin(), lines.end(), eq), lines.end());
		}

		for (const auto& l : lines) {
			std::fwrite(l.data(), 1, l.size(), stdout);
			std::fputc('\n', stdout);
		}

		std::fflush(stdout);
		return 0;
	}

	static int builtin_uniq(Executor& exec, const std::vector<std::string>& args) {
		bool count = false, dups_only = false, uniques_only = false, fold = false;
		std::vector<std::string> files;
		for (const auto& a : args) {
			if (a == "-c" || a == "--count") count = true;
			else if (a == "-d" || a == "--repeated") dups_only = true;
			else if (a == "-u" || a == "--unique") uniques_only = true;
			else if (a == "-i" || a == "--ignore-case") fold = true;
			else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					switch (a[k]) {
					case 'c': count = true; break;
					case 'd': dups_only = true; break;
					case 'u': uniques_only = true; break;
					case 'i': fold = true; break;
					}
				}
			}
			else files.push_back(a);
		}

		if (files.empty()) files.push_back("-");

		auto eq = [&](const std::string& a, const std::string& b) {
			return fold ? equalFold(a, b) : a == b;
		};

		std::vector<std::string> lines;
		for (const auto& f : files) {
			if (!readAllLines(exec, f, lines)) {
				perr("uniq", f + ": " + std::strerror(errno));
				return 1;
			}
		}

		std::size_t i = 0;
		while (i < lines.size()) {
			std::size_t j = i + 1;
			while (j < lines.size() && eq(lines[j], lines[i])) ++j;
			int n = static_cast<int>(j - i);
			bool emit = true;
			if (dups_only && n < 2) emit = false;
			if (uniques_only && n > 1) emit = false;
			if (emit) {
				if (count) std::printf("%7d %s\n", n, lines[i].c_str());
				else      std::printf("%s\n", lines[i].c_str());
			}

			i = j;
		}

		std::fflush(stdout);
		return 0;
	}

	static std::string trExpandSet(const std::string& s) {
		std::string r;
		for (std::size_t i = 0; i < s.size(); ++i) {
			if (s[i] == '\\' && i + 1 < s.size()) {
				char nx = s[++i];
				switch (nx) {
				case 'n': r.push_back('\n'); break;
				case 't': r.push_back('\t'); break;
				case 'r': r.push_back('\r'); break;
				case '\\': r.push_back('\\'); break;
				case 'a': r.push_back('\a'); break;
				case 'b': r.push_back('\b'); break;
				case '0': r.push_back('\0'); break;
				default: r.push_back(nx); break;
				}
			}
			else if (i + 2 < s.size() && s[i + 1] == '-') {
				char a = s[i], b = s[i + 2];
				if (a <= b) for (char c = a; c <= b; ++c) r.push_back(c);
				else        for (char c = a; c >= b; --c) r.push_back(c);
				i += 2;
			}
			else {
				r.push_back(s[i]);
			}
		}

		return r;
	}

	static std::vector<bool> buildTrMembership(const std::string& s1, bool complement) {
		std::vector<bool> in_s1(256, false);
		for (unsigned char c : s1) in_s1[c] = true;
		if (!complement) return in_s1;

		std::vector<bool> inv(256, true);
		for (int k = 0; k < 256; ++k) if (in_s1[k]) inv[k] = false;
		return inv;
	}

	static std::vector<unsigned char> buildTrMap(const std::string& s1,
		const std::string& s2, const std::vector<bool>& in_s1, bool complement) {
		std::vector<unsigned char> map(256);
		for (int k = 0; k < 256; ++k) map[k] = static_cast<unsigned char>(k);
		if (s2.empty()) return map;

		if (complement) {
			unsigned char repl = static_cast<unsigned char>(s2.back());
			for (int k = 0; k < 256; ++k) if (in_s1[k]) map[k] = repl;
			return map;
		}

		for (std::size_t k = 0; k < s1.size(); ++k) {
			unsigned char src = static_cast<unsigned char>(s1[k]);
			unsigned char dst = (k < s2.size())
				? static_cast<unsigned char>(s2[k])
				: static_cast<unsigned char>(s2.back());
			map[src] = dst;
		}

		return map;
	}

	static int builtin_tr(Executor&, const std::vector<std::string>& args) {
		bool delete_mode = false, squeeze = false, complement = false;
		std::vector<std::string> sets;
		for (const auto& a : args) {
			if (a == "-d") delete_mode = true;
			else if (a == "-s") squeeze = true;
			else if (a == "-c" || a == "-C" || a == "--complement") complement = true;
			else if (a.size() > 1 && a[0] == '-' && a != "-") {
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (a[k] == 'd') delete_mode = true;
					else if (a[k] == 's') squeeze = true;
					else if (a[k] == 'c' || a[k] == 'C') complement = true;
				}
			}
			else sets.push_back(a);
		}

		if (sets.empty() || (!delete_mode && !squeeze && sets.size() < 2)) {
			perr("tr", "usage: tr [-cds] SET1 [SET2]");
			return 2;
		}

		std::string s1 = trExpandSet(sets[0]);
		std::string s2 = (sets.size() > 1) ? trExpandSet(sets[1]) : std::string();
		std::vector<bool> in_s1 = buildTrMembership(s1, complement);
		std::vector<unsigned char> map =
			buildTrMap(s1, delete_mode ? std::string() : s2, in_s1, complement);

		int prev = -1;
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			unsigned char uc = static_cast<unsigned char>(c);
			if (delete_mode && in_s1[uc]) continue;
			unsigned char out = map[uc];
			if (squeeze && static_cast<int>(out) == prev) continue;
			std::fputc(out, stdout);
			prev = out;
		}

		std::fflush(stdout);
		return 0;
	}

	struct CutSpec {
		std::vector<std::pair<int, int>> ranges;  // [start,end], 1-indexed; -1 means "to end"
		bool parse(const std::string& spec) {
			std::size_t i = 0;
			while (i < spec.size()) {
				std::string tok;
				while (i < spec.size() && spec[i] != ',') tok.push_back(spec[i++]);
				if (i < spec.size() && spec[i] == ',') ++i;
				if (tok.empty()) continue;
				int a = 0, b = 0;
				auto dash = tok.find('-');
				if (dash == std::string::npos) {
					if (!parseInt(tok, a)) return false;
					b = a;
				}
				else if (dash == 0) {
					a = 1;
					if (!parseInt(tok.substr(1), b)) return false;
				}
				else if (dash + 1 == tok.size()) {
					if (!parseInt(tok.substr(0, dash), a)) return false;
					b = -1;
				}
				else {
					if (!parseInt(tok.substr(0, dash), a)) return false;
					if (!parseInt(tok.substr(dash + 1), b)) return false;
				}

				ranges.emplace_back(a, b);
			}

			return !ranges.empty();
		}

		bool contains(int n) const {
			for (auto& r : ranges) {
				if (n >= r.first && (r.second == -1 || n <= r.second)) return true;
			}

			return false;
		}
	};

	struct CutOptions {
		char delim = '\t';
		std::string field_spec, char_spec;
		bool only_delim_lines = false;
		std::vector<std::string> files;
	};

	static CutOptions parseCutArgs(const std::vector<std::string>& args) {
		CutOptions o;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-d" && i + 1 < args.size()) {
				if (!args[i + 1].empty()) o.delim = args[i + 1][0];
				++i;
			}
			else if (a.size() > 2 && a.compare(0, 2, "-d") == 0) o.delim = a[2];
			else if (a == "-f" && i + 1 < args.size()) o.field_spec = args[++i];
			else if (a.size() > 2 && a.compare(0, 2, "-f") == 0) o.field_spec = a.substr(2);
			else if (a == "-c" && i + 1 < args.size()) o.char_spec = args[++i];
			else if (a.size() > 2 && a.compare(0, 2, "-c") == 0) o.char_spec = a.substr(2);
			else if (a == "-s") o.only_delim_lines = true;
			else if (a == "--") {
				for (++i; i < args.size(); ++i) o.files.push_back(args[i]);
			}
			else if (!a.empty() && a[0] != '-') o.files.push_back(a);
		}

		return o;
	}

	static void cutEmitFieldLine(const std::string& line, char delim, bool only_delim_lines,
	                             const CutSpec& spec) {
		if (line.find(delim) == std::string::npos) {
			if (!only_delim_lines) std::printf("%s\n", line.c_str());
			return;
		}

		std::vector<std::string> fields;
		std::string cur;
		for (char c : line) {
			if (c == delim) { fields.push_back(std::move(cur)); cur.clear(); }
			else cur.push_back(c);
		}

		fields.push_back(std::move(cur));
		std::string out;
		bool first = true;
		for (std::size_t k = 0; k < fields.size(); ++k) {
			if (spec.contains(static_cast<int>(k + 1))) {
				if (!first) out.push_back(delim);
				out += fields[k];
				first = false;
			}
		}

		std::printf("%s\n", out.c_str());
	}

	static void cutEmitCharLine(const std::string& line, const CutSpec& spec) {
		std::string out;
		for (std::size_t k = 0; k < line.size(); ++k) {
			if (spec.contains(static_cast<int>(k + 1))) out.push_back(line[k]);
		}

		std::printf("%s\n", out.c_str());
	}

	static int builtin_cut(Executor& exec, const std::vector<std::string>& args) {
		CutOptions opt = parseCutArgs(args);
		if (opt.field_spec.empty() && opt.char_spec.empty()) {
			perr("cut", "specify -f or -c");
			return 1;
		}

		CutSpec spec;
		if (!spec.parse(opt.field_spec.empty() ? opt.char_spec : opt.field_spec)) {
			perr("cut", "bad field/char spec");
			return 1;
		}

		bool by_field = !opt.field_spec.empty();
		if (opt.files.empty()) opt.files.push_back("-");
		int rc = 0;
		for (const auto& f : opt.files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, f, lines)) {
				perr("cut", f + ": " + std::strerror(errno));
				rc = 1;
				continue;
			}

			for (auto& line : lines) {
				if (by_field) cutEmitFieldLine(line, opt.delim, opt.only_delim_lines, spec);
				else          cutEmitCharLine(line, spec);
			}
		}

		std::fflush(stdout);
		return rc;
	}

	static int builtin_tee(Executor& exec, const std::vector<std::string>& args) {
		bool append = false;
		std::vector<std::string> files;
		for (const auto& a : args) {
			if (a == "-a" || a == "--append") append = true;
			else if (a.size() > 1 && a[0] == '-' && a != "-") continue;
			else files.push_back(a);
		}

		std::vector<FILE*> outs;
		for (const auto& f : files) {
			FILE* fp = fopenNative(exec, f, append ? "ab" : "wb");
			if (!fp) {
				perr("tee", f + ": " + std::strerror(errno));
			}
			else {
				outs.push_back(fp);
			}
		}

		char buf[4096];
		while (true) {
			std::size_t got = std::fread(buf, 1, sizeof(buf), stdin);
			if (got == 0) break;
			std::fwrite(buf, 1, got, stdout);
			for (FILE* fp : outs) std::fwrite(buf, 1, got, fp);
		}

		for (FILE* fp : outs) std::fclose(fp);
		std::fflush(stdout);
		return 0;
	}

	static int builtin_paste(Executor& exec, const std::vector<std::string>& args) {
		std::string delims = "\t";
		std::vector<std::string> files;
		bool serial = false;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-d" && i + 1 < args.size()) {
				delims = args[++i];
			}
			else if (a.size() > 2 && a.compare(0, 2, "-d") == 0) {
				delims = a.substr(2);
			}
			else if (a == "-s") serial = true;
			else if (a == "--") { for (++i; i < args.size(); ++i) files.push_back(args[i]); }
			else if (!a.empty() && a[0] != '-') files.push_back(a);
		}

		if (files.empty()) files.push_back("-");
		if (serial) {
			int rc = 0;
			for (const auto& f : files) {
				std::vector<std::string> lines;
				if (!readAllLines(exec, f, lines)) { perr("paste", f); rc = 1; continue; }
				for (std::size_t k = 0; k < lines.size(); ++k) {
					if (k) std::fputc(delims.empty() ? '\t' : delims[k % delims.size()], stdout);
					std::fputs(lines[k].c_str(), stdout);
				}

				std::fputc('\n', stdout);
			}

			std::fflush(stdout);
			return rc;
		}

		std::vector<std::vector<std::string>> all;
		std::size_t maxlen = 0;
		for (const auto& f : files) {
			std::vector<std::string> v;
			if (!readAllLines(exec, f, v)) { perr("paste", f); return 1; }
			if (v.size() > maxlen) maxlen = v.size();
			all.push_back(std::move(v));
		}

		for (std::size_t r = 0; r < maxlen; ++r) {
			for (std::size_t c = 0; c < all.size(); ++c) {
				if (c) std::fputc(delims.empty() ? '\t' : delims[(c - 1) % delims.size()], stdout);
				if (r < all[c].size()) std::fputs(all[c][r].c_str(), stdout);
			}

			std::fputc('\n', stdout);
		}

		std::fflush(stdout);
		return 0;
	}

	static int builtin_tac(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> files;
		for (const auto& a : args) if (!a.empty() && a[0] != '-') files.push_back(a);
		if (files.empty()) files.push_back("-");
		int rc = 0;
		for (const auto& f : files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, f, lines)) { perr("tac", f); rc = 1; continue; }
			for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
				std::fputs(it->c_str(), stdout);
				std::fputc('\n', stdout);
			}
		}

		std::fflush(stdout);
		return rc;
	}

	static int builtin_rev(Executor& exec, const std::vector<std::string>& args) {
		std::vector<std::string> files;
		for (const auto& a : args) if (!a.empty() && a[0] != '-') files.push_back(a);
		if (files.empty()) files.push_back("-");
		int rc = 0;
		for (const auto& f : files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, f, lines)) { perr("rev", f); rc = 1; continue; }
			for (auto& l : lines) {
				std::reverse(l.begin(), l.end());
				std::fputs(l.c_str(), stdout);
				std::fputc('\n', stdout);
			}
		}

		std::fflush(stdout);
		return rc;
	}

	static int builtin_nl(Executor& exec, const std::vector<std::string>& args) {
		bool number_blank = false;
		std::vector<std::string> files;
		for (const auto& a : args) {
			if (a == "-ba") number_blank = true;
			else if (a == "-bt") number_blank = false;
			else if (!a.empty() && a[0] != '-') files.push_back(a);
		}

		if (files.empty()) files.push_back("-");
		int rc = 0;
		int n = 0;
		for (const auto& f : files) {
			std::vector<std::string> lines;
			if (!readAllLines(exec, f, lines)) { perr("nl", f); rc = 1; continue; }
			for (auto& l : lines) {
				if (l.empty() && !number_blank) {
					std::fputc('\n', stdout);
				}
				else {
					++n;
					std::fprintf(stdout, "%6d\t%s\n", n, l.c_str());
				}
			}
		}

		std::fflush(stdout);
		return rc;
	}

	namespace grep_internal {
		struct GrepOptions {
			bool icase = false;
			bool invert = false;
			bool line_no = false;
			bool count_only = false;
			bool fixed = false;
			bool list_only = false;
			bool quiet = false;
			bool recursive = false;
			bool whole_line = false;
			std::string pattern;
			std::vector<std::string> files;
		};

		struct GrepMatcher {
			const GrepOptions* opts = nullptr;
			const std::regex*  re   = nullptr;
			std::string        lower_pat;
		};
	}  // namespace grep_internal

	static bool applyGrepShortFlag(char c, grep_internal::GrepOptions& o) {
		switch (c) {
		case 'i': o.icase = true; return true;
		case 'v': o.invert = true; return true;
		case 'n': o.line_no = true; return true;
		case 'c': o.count_only = true; return true;
		case 'F': o.fixed = true; return true;
		case 'E': return true;                 // ERE — accept, no-op
		case 'l': o.list_only = true; return true;
		case 'q': o.quiet = true; return true;
		case 'r': case 'R': o.recursive = true; return true;
		case 'x': o.whole_line = true; return true;
		default:  return false;
		}
	}

	static int parseGrepArgs(const std::vector<std::string>& args,
	                         grep_internal::GrepOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "--") {
				for (++i; i < args.size(); ++i) {
					if (o.pattern.empty()) o.pattern = args[i];
					else o.files.push_back(args[i]);
				}
				break;
			}

			if (a == "-e" && i + 1 < args.size()) { o.pattern = args[++i]; continue; }
			if (a == "-i" || a == "--ignore-case")     { o.icase      = true; continue; }
			if (a == "-v" || a == "--invert-match")    { o.invert     = true; continue; }
			if (a == "-n" || a == "--line-number")     { o.line_no    = true; continue; }
			if (a == "-c" || a == "--count")           { o.count_only = true; continue; }
			if (a == "-F" || a == "--fixed-strings")   { o.fixed      = true; continue; }
			if (a == "-E" || a == "--extended-regexp") continue;
			if (a == "-l") { o.list_only = true; continue; }
			if (a == "-q" || a == "--quiet" || a == "--silent") { o.quiet = true; continue; }
			if (a == "-r" || a == "-R" || a == "--recursive") { o.recursive = true; continue; }
			if (a == "-x" || a == "--line-regexp") { o.whole_line = true; continue; }

			if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
				for (std::size_t k = 1; k < a.size(); ++k) {
					if (!applyGrepShortFlag(a[k], o)) {
						std::fprintf(stderr, "wbsh: grep: unknown option -%c\n", a[k]);
						return 2;
					}
				}
				continue;
			}

			if (o.pattern.empty()) o.pattern = a;
			else o.files.push_back(a);
		}

		return 0;
	}

	static std::vector<std::string> expandGrepRecursive(Executor& exec,
	                                                    const std::vector<std::string>& files) {
		std::vector<std::string> out;
		for (const auto& f : files) {
			if (f == "-") { out.push_back(f); continue; }
			fs::path nat = toNative(exec, f);
			std::error_code ec;
			if (!fs::is_directory(nat, ec)) {
				out.push_back(f);
				continue;
			}

			fs::recursive_directory_iterator it(nat,
				fs::directory_options::skip_permission_denied, ec);
			if (ec) continue;
			for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
				if (ec) break;
				std::error_code fec;
				if (cur->is_regular_file(fec)) {
					std::string p = pathToUtf8(cur->path());
					std::replace(p.begin(), p.end(), '\\', '/');
					out.push_back(std::move(p));
				}
			}
		}

		return out;
	}

	static void prepareGrepMatcher(const grep_internal::GrepOptions& o,
	                               const std::regex& re,
	                               grep_internal::GrepMatcher& out) {
		out.opts = &o;
		out.re   = &re;
		out.lower_pat.clear();
		if (o.fixed && o.icase) {
			out.lower_pat = o.pattern;
			for (char& c : out.lower_pat)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
	}

	static bool matchesGrepLine(const grep_internal::GrepMatcher& m,
	                            const std::string& line) {
		const grep_internal::GrepOptions& o = *m.opts;
		if (o.fixed) {
			if (o.whole_line) {
				if (line.size() != o.pattern.size()) return false;
				if (!o.icase) return line == o.pattern;
				for (std::size_t k = 0; k < line.size(); ++k) {
					if (std::tolower(static_cast<unsigned char>(line[k])) !=
					    std::tolower(static_cast<unsigned char>(o.pattern[k])))
						return false;
				}

				return true;
			}

			if (!o.icase) return line.find(o.pattern) != std::string::npos;
			std::string lo = line;
			for (char& c : lo)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return lo.find(m.lower_pat) != std::string::npos;
		}

		if (o.whole_line) return std::regex_match(line, *m.re);
		return std::regex_search(line, *m.re);
	}

	static int grepOneFile(const std::string& f,
	                       const grep_internal::GrepOptions& o,
	                       const grep_internal::GrepMatcher& matcher,
	                       bool show_filename, bool* any_match, Executor& exec) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, f, lines)) {
			perr("grep", f + ": " + std::strerror(errno));
			return 2;
		}

		int file_count = 0;
		for (std::size_t k = 0; k < lines.size(); ++k) {
			bool m = matchesGrepLine(matcher, lines[k]);
			if (o.invert) m = !m;
			if (!m) continue;

			++file_count;
			*any_match = true;
			if (o.quiet) return 0;
			if (o.list_only) { std::printf("%s\n", f.c_str()); return 0; }
			if (o.count_only) continue;
			if (show_filename) std::printf("%s:", f.c_str());
			if (o.line_no)     std::printf("%zu:", k + 1);
			std::printf("%s\n", lines[k].c_str());
		}

		if (o.count_only && !o.list_only && !o.quiet) {
			if (show_filename) std::printf("%s:", f.c_str());
			std::printf("%d\n", file_count);
		}

		return 0;
	}

	static int builtin_grep(Executor& exec, const std::vector<std::string>& args) {
		grep_internal::GrepOptions o;
		const int parse_rc = parseGrepArgs(args, o);
		if (parse_rc != 0) return parse_rc;

		if (o.pattern.empty()) { perr("grep", "missing pattern"); return 2; }
		if (o.files.empty())   o.files.push_back(o.recursive ? "." : "-");
		if (o.recursive)       o.files = expandGrepRecursive(exec, o.files);

		std::regex re;
		if (!o.fixed) {
			std::regex::flag_type flags = std::regex::ECMAScript;
			if (o.icase) flags |= std::regex::icase;
			if (!compileRegex(re, o.pattern, flags)) {
				perr("grep", "bad pattern: " + o.pattern);
				return 2;
			}
		}

		grep_internal::GrepMatcher matcher;
		prepareGrepMatcher(o, re, matcher);

		bool any_match = false;
		const bool show_filename = o.files.size() > 1;
		int rc = 1;
		for (const auto& f : o.files) {
			const int file_rc = grepOneFile(f, o, matcher, show_filename, &any_match, exec);
			if (file_rc == 2) rc = 2;
		}

		std::fflush(stdout);
		if (o.quiet) return any_match ? 0 : 1;
		return any_match ? 0 : rc;
	}

	namespace find_internal {
		struct FindOptions {
			std::vector<std::string> roots;
			std::string name_pat;
			char type_filter = 0;   // 0 = any, 'f', 'd', 'l'
			int max_depth = -1;
		};
	}  // namespace find_internal

	static int parseFindArgs(const std::vector<std::string>& args,
	                         find_internal::FindOptions& o) {
		std::size_t i = 0;
		while (i < args.size()) {
			const std::string& a = args[i];
			if (a == "-name" && i + 1 < args.size()) {
				o.name_pat = args[++i];
				++i;
				continue;
			}

			if (a == "-type" && i + 1 < args.size()) {
				if (!args[i + 1].empty()) o.type_filter = args[i + 1][0];
				i += 2;
				continue;
			}

			if (a == "-maxdepth" && i + 1 < args.size()) {
				if (!parseInt(args[++i], o.max_depth)) {
					perr("find", "bad -maxdepth");
					return 1;
				}

				++i;
				continue;
			}

			if (!a.empty() && a[0] == '-') {
				std::fprintf(stderr,
					"wbsh: find: option %s not implemented\n", a.c_str());
				return 1;
			}

			o.roots.push_back(a);
			++i;
		}

		if (o.roots.empty()) o.roots.push_back(".");
		return 0;
	}

	static bool findNameMatches(const std::string& name_pat, const std::string& name) {
		if (name_pat.empty()) return true;
		std::string r;
		for (char c : name_pat) {
			switch (c) {
			case '*': r += ".*"; break;
			case '?': r += ".";  break;
			case '.': case '+': case '(': case ')': case '|':
			case '^': case '$': case '{': case '}': case '\\':
				r.push_back('\\');
				r.push_back(c);
				break;
			default:  r.push_back(c); break;
			}
		}

		std::regex re;
		if (!compileRegex(re, "^(?:" + r + ")$")) return false;
		return searchRegex(name, re);
	}

	static bool findTypeMatches(char type_filter, const fs::path& p) {
		if (type_filter == 0) return true;
		std::error_code ec;
		const auto st = fs::symlink_status(p, ec);
		if (ec) return false;
		if (type_filter == 'l') return fs::is_symlink(st);
		if (type_filter == 'd') return fs::is_directory(st);
		if (type_filter == 'f') return fs::is_regular_file(st);
		return true;
	}

	static void walkAndPrintFindMatches(Executor& exec,
	                                    const find_internal::FindOptions& o,
	                                    const fs::path& nat,
	                                    const std::string& root_arg) {
		std::error_code ec;
		const std::string root_basename = pathToUtf8(nat.filename()).empty()
			? root_arg
			: pathToUtf8(nat.filename());
		if (findTypeMatches(o.type_filter, nat)
		    && findNameMatches(o.name_pat, root_basename)) {
			std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(nat)).c_str());
		}

		if (!fs::is_directory(nat, ec)) return;

		fs::recursive_directory_iterator it(nat,
			fs::directory_options::skip_permission_denied, ec);
		if (ec) return;
		for (auto cur = it; cur != fs::recursive_directory_iterator(); cur.increment(ec)) {
			if (ec) break;
			if (o.max_depth >= 0 && cur.depth() >= o.max_depth) {
				cur.disable_recursion_pending();
			}

			const fs::path p = cur->path();
			if (!findTypeMatches(o.type_filter, p)) continue;
			if (!findNameMatches(o.name_pat, pathToUtf8(p.filename()))) continue;
			std::printf("%s\n", exec.pathConv().toPosix(pathToUtf8(p)).c_str());
		}
	}

	static int builtin_find(Executor& exec, const std::vector<std::string>& args) {
		find_internal::FindOptions o;
		const int parse_rc = parseFindArgs(args, o);
		if (parse_rc != 0) return parse_rc;

		int rc = 0;
		for (const auto& r : o.roots) {
			std::error_code ec;
			const fs::path nat = toNative(exec, r);
			if (!fs::exists(nat, ec)) {
				std::fprintf(stderr,
					"wbsh: find: %s: no such file or directory\n", r.c_str());
				rc = 1;
				continue;
			}

			walkAndPrintFindMatches(exec, o, nat, r);
		}

		std::fflush(stdout);
		return rc;
	}

	// sed subset: `s/PAT/REPL/[g]` commands only.
	struct SedSubst {
		std::regex re;
		std::string repl;
		bool global = false;
	};

	// BRE patterns are translated to ERE and compiled under
	// std::regex::extended — MSVC's std::regex::basic engine has
	// greediness bugs with back-to-back `[^X]*` runs.
	static std::string translateBreToErePattern(const std::string& bre) {
		std::string out;
		out.reserve(bre.size());
		bool in_class = false;
		bool class_start = false;
		for (std::size_t i = 0; i < bre.size(); ++i) {
			const char c = bre[i];
			if (in_class) {
				out.push_back(c);
				if (c == ']' && !class_start) { in_class = false; }
				class_start = false;
				continue;
			}

			if (c == '[') {
				in_class = true;
				class_start = true;
				out.push_back(c);
				if (i + 1 < bre.size() && bre[i + 1] == '^') {
					out.push_back('^');
					++i;
				}
				continue;
			}

			if (c == '\\' && i + 1 < bre.size()) {
				const char nx = bre[i + 1];
				if (nx == '(' || nx == ')' || nx == '{' || nx == '}'
				    || nx == '|') {
					out.push_back(nx);
					++i;
					continue;
				}

				out.push_back(c);
				out.push_back(nx);
				++i;
				continue;
			}

			if (c == '(' || c == ')' || c == '{' || c == '}' || c == '|') {
				out.push_back('\\');
				out.push_back(c);
				continue;
			}

			if (c == '?' || c == '+') {
				out.push_back('\\');
				out.push_back(c);
				continue;
			}

			out.push_back(c);
		}

		return out;
	}

	static bool sedScanPattern(const std::string& cmd, char delim, std::size_t& i,
	                           std::string& pat, std::string& err) {
		while (i < cmd.size() && cmd[i] != delim) {
			if (cmd[i] == '\\' && i + 1 < cmd.size()) {
				pat.push_back(cmd[i]);
				pat.push_back(cmd[i + 1]);
				i += 2;
				continue;
			}

			pat.push_back(cmd[i++]);
		}

		if (i >= cmd.size()) { err = "missing closing delimiter"; return false; }
		++i;
		return true;
	}

	static void sedScanReplacement(const std::string& cmd, char delim, std::size_t& i,
	                               std::string& rep) {
		while (i < cmd.size() && cmd[i] != delim) {
			if (cmd[i] == '\\' && i + 1 < cmd.size()) {
				char nx = cmd[i + 1];
				if (nx == 'n') { rep.push_back('\n'); i += 2; continue; }
				if (nx == 't') { rep.push_back('\t'); i += 2; continue; }
				if (nx >= '0' && nx <= '9') {
					rep.push_back('$'); rep.push_back(nx);
					i += 2;
					continue;
				}

				rep.push_back(nx);
				i += 2;
				continue;
			}

			if (cmd[i] == '$') { rep += "$$"; ++i; continue; }
			rep.push_back(cmd[i++]);
		}

		if (i < cmd.size()) ++i;
	}

	static bool sedParseSubst(const std::string& cmd, bool extended,
	                          SedSubst& out, std::string& err) {
		if (cmd.size() < 4 || cmd[0] != 's') {
			err = "only s/PAT/REPL/[g] is supported";
			return false;
		}

		char delim = cmd[1];
		std::size_t i = 2;
		std::string pat, rep;
		if (!sedScanPattern(cmd, delim, i, pat, err)) return false;
		sedScanReplacement(cmd, delim, i, rep);
		std::string flags = (i < cmd.size()) ? cmd.substr(i) : std::string();
		const std::string compiled_pat = extended ? pat : translateBreToErePattern(pat);
		if (!compileRegex(out.re, compiled_pat, std::regex::extended)) {
			err = "regex: invalid pattern: " + pat;
			return false;
		}

		out.repl = rep;
		out.global = flags.find('g') != std::string::npos;
		return true;
	}

	static void parseSedArgs(const std::vector<std::string>& args,
	                         bool& quiet, bool& extended, std::string& script,
	                         std::vector<std::string>& files) {
		auto append_script = [&](const std::string& s) {
			if (!script.empty()) script.push_back(';');
			script += s;
		};
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-n" || a == "--quiet" || a == "--silent") { quiet = true; continue; }
			if (a == "-E" || a == "-r" || a == "--regexp-extended") {
				extended = true;
				continue;
			}

			if (a == "--") {
				for (++i; i < args.size(); ++i) files.push_back(args[i]);
				break;
			}

			if (a == "-e" && i + 1 < args.size()) {
				append_script(args[++i]);
				continue;
			}

			if (a.size() > 2 && a.compare(0, 2, "-e") == 0) {
				append_script(a.substr(2));
				continue;
			}

			if (script.empty() && (a.empty() || a[0] != '-')) {
				script = a;
				continue;
			}

			files.push_back(a);
		}
	}

	static bool compileSedScript(const std::string& script, bool extended,
	                             std::vector<SedSubst>& out_cmds) {
		std::size_t s = 0;
		while (s <= script.size()) {
			std::size_t e = script.find(';', s);
			if (e == std::string::npos) e = script.size();
			const std::string sub = script.substr(s, e - s);
			if (!sub.empty()) {
				SedSubst c;
				std::string err;
				if (!sedParseSubst(sub, extended, c, err)) {
					std::fprintf(stderr, "wbsh: sed: %s\n", err.c_str());
					return false;
				}

				out_cmds.push_back(std::move(c));
			}

			if (e == script.size()) break;
			s = e + 1;
		}

		return true;
	}

	static std::string applySedCommandsToLine(const std::vector<SedSubst>& cmds,
	                                          const std::string& line) {
		std::string out = line;
		for (const auto& c : cmds) {
			out = c.global
				? std::regex_replace(out, c.re, c.repl)
				: std::regex_replace(out, c.re, c.repl,
				                     std::regex_constants::format_first_only);
		}

		return out;
	}

	static int runSedOnFile(FILE* fp, const std::vector<SedSubst>& cmds, bool quiet) {
		std::string line;
		int c;
		while ((c = std::fgetc(fp)) != EOF) {
			if (c == '\n') {
				const std::string out = applySedCommandsToLine(cmds, line);
				if (!quiet) {
					std::fwrite(out.data(), 1, out.size(), stdout);
					std::fputc('\n', stdout);
				}

				line.clear();
				continue;
			}

			line.push_back(static_cast<char>(c));
		}

		if (!line.empty()) {
			const std::string out = applySedCommandsToLine(cmds, line);
			if (!quiet) std::fwrite(out.data(), 1, out.size(), stdout);
		}

		return 0;
	}

	static int builtin_sed(Executor& exec, const std::vector<std::string>& args) {
		bool quiet = false;
		bool extended = false;
		std::string script;
		std::vector<std::string> files;
		parseSedArgs(args, quiet, extended, script, files);
		if (script.empty()) { perr("sed", "no script provided"); return 2; }

		std::vector<SedSubst> cmds;
		if (!compileSedScript(script, extended, cmds)) return 2;
		if (files.empty()) files.push_back("-");

		int rc = 0;
		for (const auto& f : files) {
			FILE* fp = (f == "-") ? stdin : fopenNative(exec, f, "rb");
			if (!fp) {
				perr("sed", f + ": " + std::strerror(errno));
				rc = 1;
				continue;
			}

			runSedOnFile(fp, cmds, quiet);
			if (fp != stdin) std::fclose(fp);
		}

		std::fflush(stdout);
		return rc;
	}

	struct FoldOptions {
		int width = 80;
		bool wrap_spaces = false;
		std::vector<std::string> files;
	};

	static bool parseFoldArgs(const std::vector<std::string>& args, FoldOptions& o) {
		bool by_bytes = true;   // -b vs -c: same in the ASCII path
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-b" || a == "--bytes") by_bytes = true;
			else if (a == "-c" || a == "--characters") by_bytes = false;
			else if (a == "-s" || a == "--spaces") o.wrap_spaces = true;
			else if (a == "-w" && i + 1 < args.size()) {
				parseInt(args[++i], o.width);
			}
			else if (a.size() > 2 && a.compare(0, 2, "-w") == 0) {
				parseInt(a.substr(2), o.width);
			}
			else if (!a.empty() && a[0] == '-' && a != "-" && std::isdigit((unsigned char)a[1])) {
				parseInt(a.substr(1), o.width);
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("fold", "unknown option: " + a);
				return false;
			}
			else o.files.push_back(a);
		}

		(void)by_bytes;
		if (o.width <= 0) { perr("fold", "width must be > 0"); return false; }
		return true;
	}

	static void foldEmitLine(const std::string& line, int width, bool wrap_spaces) {
		std::size_t pos = 0;
		while (pos < line.size()) {
			std::size_t take = (std::min<std::size_t>)(width, line.size() - pos);
			if (wrap_spaces && take < line.size() - pos) {
				auto sp = line.rfind(' ', pos + take - 1);
				if (sp != std::string::npos && sp >= pos) take = sp - pos + 1;
			}

			std::fwrite(line.data() + pos, 1, take, stdout);
			std::putchar('\n');
			pos += take;
		}

		if (line.empty()) std::putchar('\n');
	}

	static void foldRunOnStream(FILE* f, int width, bool wrap_spaces) {
		std::string buf;
		int c;
		while ((c = std::fgetc(f)) != EOF) {
			if (c == '\n') { foldEmitLine(buf, width, wrap_spaces); buf.clear(); }
			else buf.push_back((char)c);
		}

		if (!buf.empty()) foldEmitLine(buf, width, wrap_spaces);
	}

	static int builtin_fold(Executor& exec, const std::vector<std::string>& args) {
		FoldOptions o;
		if (!parseFoldArgs(args, o)) return 1;
		if (o.files.empty() || o.files[0] == "-") {
			foldRunOnStream(stdin, o.width, o.wrap_spaces);
			return 0;
		}

		for (const auto& p : o.files) {
			FILE* f = fopenNative(exec, p, "rb");
			if (!f) { perr("fold", p, std::error_code(errno, std::system_category())); return 1; }
			foldRunOnStream(f, o.width, o.wrap_spaces);
			std::fclose(f);
		}

		return 0;
	}

	struct ColumnOptions {
		bool table = false;
		std::string sep = " \t";
		std::string out_sep = "  ";
		std::vector<std::string> files;
	};

	static bool parseColumnArgs(const std::vector<std::string>& args, ColumnOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-t" || a == "--table") o.table = true;
			else if ((a == "-s" || a == "--separator") && i + 1 < args.size()) o.sep = args[++i];
			else if (a.size() > 2 && a.compare(0, 2, "-s") == 0) o.sep = a.substr(2);
			else if ((a == "-o" || a == "--output-separator") && i + 1 < args.size()) {
				o.out_sep = args[++i];
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("column", "unknown option: " + a);
				return false;
			}
			else o.files.push_back(a);
		}

		return true;
	}

	static std::vector<std::string> columnSplitLine(const std::string& line,
	                                                const std::string& sep) {
		std::vector<std::string> fields;
		std::string cur;
		for (char c : line) {
			if (sep.find(c) != std::string::npos) {
				fields.push_back(std::move(cur)); cur.clear();
				if (sep == " \t") {
					while (!fields.empty() && fields.back().empty()) fields.pop_back();
				}
			}
			else cur.push_back(c);
		}

		fields.push_back(std::move(cur));
		return fields;
	}

	static void columnReadStream(FILE* f, const std::string& sep,
	                             std::vector<std::vector<std::string>>& rows) {
		std::string buf;
		int c;
		while ((c = std::fgetc(f)) != EOF) {
			if (c == '\n') { rows.push_back(columnSplitLine(buf, sep)); buf.clear(); }
			else buf.push_back((char)c);
		}

		if (!buf.empty()) rows.push_back(columnSplitLine(buf, sep));
	}

	static void columnEmitPlain(const std::vector<std::vector<std::string>>& rows) {
		for (const auto& row : rows) {
			for (std::size_t i = 0; i < row.size(); ++i) {
				if (i) std::fputs(" ", stdout);
				std::fputs(row[i].c_str(), stdout);
			}

			std::putchar('\n');
		}
	}

	static void columnEmitTable(const std::vector<std::vector<std::string>>& rows,
	                            const std::string& out_sep) {
		std::size_t cols = 0;
		for (const auto& r : rows) cols = (std::max)(cols, r.size());
		std::vector<std::size_t> widths(cols, 0);
		for (const auto& r : rows) {
			for (std::size_t i = 0; i < r.size(); ++i)
				widths[i] = (std::max)(widths[i], r[i].size());
		}

		for (const auto& r : rows) {
			for (std::size_t i = 0; i < r.size(); ++i) {
				if (i) std::fputs(out_sep.c_str(), stdout);
				std::fputs(r[i].c_str(), stdout);
				if (i + 1 < r.size()) {
					for (std::size_t k = r[i].size(); k < widths[i]; ++k) std::putchar(' ');
				}
			}

			std::putchar('\n');
		}
	}

	static int builtin_column(Executor& exec, const std::vector<std::string>& args) {
		ColumnOptions o;
		if (!parseColumnArgs(args, o)) return 1;

		std::vector<std::vector<std::string>> rows;
		if (o.files.empty() || o.files[0] == "-") columnReadStream(stdin, o.sep, rows);
		else {
			for (const auto& p : o.files) {
				FILE* f = fopenNative(exec, p, "rb");
				if (!f) {
					perr("column", p, std::error_code(errno, std::system_category()));
					return 1;
				}

				columnReadStream(f, o.sep, rows);
				std::fclose(f);
			}
		}

		if (o.table) columnEmitTable(rows, o.out_sep);
		else         columnEmitPlain(rows);
		return 0;
	}

	static int builtin_expand(Executor& exec, const std::vector<std::string>& args) {
		int tabstop = 8;
		std::vector<std::string> files;
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if ((a == "-t" || a == "--tabs") && i + 1 < args.size()) {
				parseInt(args[++i], tabstop);
			}
			else if (a.size() > 2 && a.compare(0, 2, "-t") == 0) {
				parseInt(a.substr(2), tabstop);
			}
			else if (!a.empty() && a[0] == '-' && a != "-" && std::isdigit((unsigned char)a[1])) {
				parseInt(a.substr(1), tabstop);
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("expand", "unknown option: " + a); return 1;
			}
			else files.push_back(a);
		}

		if (tabstop <= 0) tabstop = 8;
		auto runOn = [&](FILE* f) {
			int col = 0; int c;
			while ((c = std::fgetc(f)) != EOF) {
				if (c == '\t') {
					int spaces = tabstop - (col % tabstop);
					for (int i = 0; i < spaces; ++i) std::putchar(' ');
					col += spaces;
				}
				else if (c == '\n') { std::putchar('\n'); col = 0; }
				else { std::putchar(c); ++col; }
			}
			};
		if (files.empty() || files[0] == "-") runOn(stdin);
		else {
			for (const auto& p : files) {
				FILE* f = fopenNative(exec, p, "rb");
				if (!f) {
					perr("expand", p, std::error_code(errno, std::system_category()));
					return 1;
				}

				runOn(f);
				std::fclose(f);
			}
		}

		return 0;
	}

	struct UnexpandOptions {
		int tabstop = 8;
		bool all = false;
		std::vector<std::string> files;
	};

	static bool parseUnexpandArgs(const std::vector<std::string>& args, UnexpandOptions& o) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-a" || a == "--all") o.all = true;
			else if ((a == "-t" || a == "--tabs") && i + 1 < args.size()) {
				parseInt(args[++i], o.tabstop);
			}
			else if (a.size() > 2 && a.compare(0, 2, "-t") == 0) {
				parseInt(a.substr(2), o.tabstop);
			}
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("unexpand", "unknown option: " + a);
				return false;
			}
			else o.files.push_back(a);
		}

		if (o.tabstop <= 0) o.tabstop = 8;
		return true;
	}

	static void unexpandEmitLine(const std::string& line, int tabstop, bool all) {
		std::size_t i = 0;
		int col = 0;
		bool past_indent = false;
		while (i < line.size()) {
			if (!past_indent || all) {
				int run_col = col;
				while (i < line.size() && line[i] == ' ') { ++i; ++col; }
				if (col > run_col) {
					int next_tab = ((run_col / tabstop) + 1) * tabstop;
					while (next_tab <= col) {
						std::putchar('\t');
						run_col = next_tab;
						next_tab += tabstop;
					}

					while (run_col < col) { std::putchar(' '); ++run_col; }
					continue;
				}
			}

			char ch = line[i++];
			if (ch != ' ' && ch != '\t') past_indent = true;
			std::putchar(ch);
			if (ch == '\t') col = ((col / tabstop) + 1) * tabstop;
			else ++col;
		}
	}

	static void unexpandRunOnStream(FILE* f, int tabstop, bool all) {
		std::string buf;
		int c;
		while ((c = std::fgetc(f)) != EOF) {
			if (c == '\n') {
				unexpandEmitLine(buf, tabstop, all);
				std::putchar('\n');
				buf.clear();
			}
			else buf.push_back((char)c);
		}

		if (!buf.empty()) unexpandEmitLine(buf, tabstop, all);
	}

	static int builtin_unexpand(Executor& exec, const std::vector<std::string>& args) {
		UnexpandOptions o;
		if (!parseUnexpandArgs(args, o)) return 1;
		if (o.files.empty() || o.files[0] == "-") {
			unexpandRunOnStream(stdin, o.tabstop, o.all);
			return 0;
		}

		for (const auto& p : o.files) {
			FILE* f = fopenNative(exec, p, "rb");
			if (!f) {
				perr("unexpand", p, std::error_code(errno, std::system_category()));
				return 1;
			}

			unexpandRunOnStream(f, o.tabstop, o.all);
			std::fclose(f);
		}

		return 0;
	}

	static int builtin_comm(Executor& exec, const std::vector<std::string>& args) {
		bool suppress[3] = { false, false, false };  // -1 -2 -3
		std::vector<std::string> files;
		for (const auto& a : args) {
			if (a == "-1") suppress[0] = true;
			else if (a == "-2") suppress[1] = true;
			else if (a == "-3") suppress[2] = true;
			else if (a == "-12" || a == "-21") { suppress[0] = suppress[1] = true; }
			else if (a == "-13" || a == "-31") { suppress[0] = suppress[2] = true; }
			else if (a == "-23" || a == "-32") { suppress[1] = suppress[2] = true; }
			else if (a == "-123") { suppress[0] = suppress[1] = suppress[2] = true; }
			else if (!a.empty() && a[0] == '-' && a != "-") {
				perr("comm", "unknown option: " + a); return 1;
			}
			else files.push_back(a);
		}

		if (files.size() != 2) { perr("comm", "usage: comm [opts] FILE1 FILE2"); return 1; }
		auto loadLines = [&](const std::string& p) -> std::vector<std::string> {
			std::vector<std::string> out;
			FILE* f = (p == "-") ? stdin
				: fopenNative(exec, p, "rb");
			if (!f) { perr("comm", p, std::error_code(errno, std::system_category())); return out; }
			std::string buf; int c;
			while ((c = std::fgetc(f)) != EOF) {
				if (c == '\n') { out.push_back(std::move(buf)); buf.clear(); }
				else buf.push_back((char)c);
			}

			if (!buf.empty()) out.push_back(std::move(buf));
			if (f != stdin) std::fclose(f);
			return out;
			};
		auto la = loadLines(files[0]);
		auto lb = loadLines(files[1]);
		auto emit = [&](int col, const std::string& s) {
			if (suppress[col]) return;
			for (int k = 0; k < col; ++k) std::putchar('\t');
			std::fputs(s.c_str(), stdout);
			std::putchar('\n');
			};
		std::size_t i = 0, j = 0;
		while (i < la.size() && j < lb.size()) {
			if (la[i] == lb[j]) { emit(2, la[i]); ++i; ++j; }
			else if (la[i] < lb[j]) { emit(0, la[i]); ++i; }
			else { emit(1, lb[j]); ++j; }
		}

		while (i < la.size()) { emit(0, la[i++]); }
		while (j < lb.size()) { emit(1, lb[j++]); }
		return 0;
	}

	static bool parseXargsArgs(const std::vector<std::string>& args,
	                           int& n_per, std::vector<std::string>& cmd) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& a = args[i];
			if (a == "-n" && i + 1 < args.size()) {
				if (!parseInt(args[++i], n_per)) return false;
				continue;
			}

			if (a.size() > 2 && a.compare(0, 2, "-n") == 0) {
				if (!parseInt(a.substr(2), n_per)) return false;
				continue;
			}

			if (a == "--") {
				for (++i; i < args.size(); ++i) cmd.push_back(args[i]);
				break;
			}

			cmd.push_back(a);
		}

		return true;
	}

	static std::vector<std::string> xargsReadItems() {
		std::vector<std::string> items;
		std::string cur;
		int c;
		while ((c = std::fgetc(stdin)) != EOF) {
			if (std::isspace(static_cast<unsigned char>(c))) {
				if (!cur.empty()) { items.push_back(std::move(cur)); cur.clear(); }
			}
			else cur.push_back(static_cast<char>(c));
		}

		if (!cur.empty()) items.push_back(std::move(cur));
		return items;
	}

	static std::string xargsQuoteArgv(const std::vector<std::string>& argv) {
		std::string line;
		for (std::size_t k = 0; k < argv.size(); ++k) {
			if (k) line.push_back(' ');
			line.push_back('\'');
			for (char ch : argv[k]) {
				if (ch == '\'') line += "'\\''";
				else line.push_back(ch);
			}

			line.push_back('\'');
		}

		return line;
	}

	static int xargsInvokeBatch(Executor& exec, const std::vector<std::string>& cmd,
	                            std::vector<std::string> batch, bool skip_empty) {
		if (batch.empty() && skip_empty) return 0;
		std::vector<std::string> argv = cmd;
		for (auto& it : batch) argv.push_back(std::move(it));
		if (argv.empty()) return 0;
		if (exec.isFunction(argv[0]) || exec.isBuiltin(argv[0])) {
			std::vector<std::string> a(argv.begin() + 1, argv.end());
			return exec.isBuiltin(argv[0])
				? exec.callBuiltin(argv[0], a)
				: exec.callFunction(argv[0], a);
		}

		return exec.executeText(xargsQuoteArgv(argv), "<xargs>");
	}

	static int builtin_xargs(Executor& exec, const std::vector<std::string>& args) {
		int n_per = -1;   // -1 means "all in one batch"
		std::vector<std::string> cmd;
		if (!parseXargsArgs(args, n_per, cmd)) return 1;
		if (cmd.empty()) cmd.push_back("echo");

		std::vector<std::string> items = xargsReadItems();
		int rc = 0;
		if (n_per > 0) {
			for (std::size_t k = 0; k < items.size(); k += static_cast<std::size_t>(n_per)) {
				std::vector<std::string> batch;
				for (int j = 0; j < n_per && k + j < items.size(); ++j) {
					batch.push_back(items[k + j]);
				}

				int r = xargsInvokeBatch(exec, cmd, std::move(batch), /*skip_empty=*/false);
				if (r != 0) rc = r;
			}
		} else {
			int r = xargsInvokeBatch(exec, cmd, items, /*skip_empty=*/true);
			if (r != 0) rc = r;
		}

		return rc;
	}

	void registerTextBuiltins(Executor& exec) {
		exec.registerBuiltin("sort",     builtin_sort);
		exec.registerBuiltin("uniq",     builtin_uniq);
		exec.registerBuiltin("tr",       builtin_tr);
		exec.registerBuiltin("cut",      builtin_cut);
		exec.registerBuiltin("tee",      builtin_tee);
		exec.registerBuiltin("paste",    builtin_paste);
		exec.registerBuiltin("tac",      builtin_tac);
		exec.registerBuiltin("rev",      builtin_rev);
		exec.registerBuiltin("nl",       builtin_nl);
		exec.registerBuiltin("fold",     builtin_fold);
		exec.registerBuiltin("column",   builtin_column);
		exec.registerBuiltin("expand",   builtin_expand);
		exec.registerBuiltin("unexpand", builtin_unexpand);
		exec.registerBuiltin("comm",     builtin_comm);
		exec.registerBuiltin("grep",     builtin_grep);
		exec.registerBuiltin("find",     builtin_find);
		exec.registerBuiltin("xargs",    builtin_xargs);
		exec.registerBuiltin("sed",      builtin_sed);
		exec.registerBuiltin("awk",      builtin_awk);
		exec.registerBuiltin("gawk",     builtin_awk);
	}

}  // namespace wbsh
