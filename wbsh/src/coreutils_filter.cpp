/**
 * @file coreutils_filter.cpp
 * @brief Line and byte filters: join, shuf, tsort, strings.
 */

#include "coreutils_internal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "executor.h"
#include "numparse.h"

namespace wbsh {

	namespace filter_detail {

		struct OutputField {
			int file = 0;
			std::size_t field = 0;
		};

		struct JoinOptions {
			char separator = '\0';
			std::size_t field1 = 1;
			std::size_t field2 = 1;
			bool unpaired[3] = { false, false, false };
			bool only_unpaired = false;
			bool ignore_case = false;
			std::string empty;
			std::vector<OutputField> format;
			std::vector<std::string> files;
		};

		struct JoinLine {
			std::vector<std::string> fields;
			std::string key;
		};

		struct GroupRange {
			std::size_t begin = 0;
			std::size_t end = 0;
		};

		struct ShufOptions {
			bool echo_args = false;
			bool repeat = false;
			bool zero_terminated = false;
			bool have_range = false;
			long long low = 0;
			long long high = 0;
			unsigned long long count = ~0ULL;
			std::string output;
			std::vector<std::string> operands;
		};

		struct StringsOptions {
			std::size_t min_length = 4;
			char radix = '\0';
			bool show_name = false;
			std::vector<std::string> files;
		};

		struct TsortGraph {
			std::vector<std::string> names;
			std::unordered_map<std::string, std::size_t> index;
			std::vector<std::vector<std::size_t>> edges;
			std::vector<std::size_t> indegree;
		};

	}  // namespace filter_detail

	using filter_detail::GroupRange;
	using filter_detail::JoinLine;
	using filter_detail::JoinOptions;
	using filter_detail::OutputField;
	using filter_detail::ShufOptions;
	using filter_detail::StringsOptions;
	using filter_detail::TsortGraph;

	static std::string lowerAscii(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static std::vector<std::string> splitOnSeparator(const std::string& line, char separator) {
		std::vector<std::string> fields;
		std::size_t start = 0;
		for (std::size_t at = line.find(separator); at != std::string::npos;
		     at = line.find(separator, start)) {
			fields.push_back(line.substr(start, at - start));
			start = at + 1;
		}

		fields.push_back(line.substr(start));
		return fields;
	}

	static std::vector<std::string> splitJoinFields(const std::string& line, char separator) {
		if (separator != '\0') return splitOnSeparator(line, separator);

		std::vector<std::string> fields;
		std::size_t at = 0;
		while (at < line.size()) {
			while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
			const std::size_t start = at;
			while (at < line.size() && line[at] != ' ' && line[at] != '\t') ++at;
			if (at > start) fields.push_back(line.substr(start, at - start));
		}

		return fields;
	}

	static bool parseOutputFields(const std::string& spec, std::vector<OutputField>& format) {
		std::size_t start = 0;
		while (start < spec.size()) {
			std::size_t end = spec.find_first_of(", ", start);
			if (end == std::string::npos) end = spec.size();
			const std::string item = spec.substr(start, end - start);
			start = end + 1;
			if (item.empty()) continue;

			OutputField field;
			int number = 0;
			if (item == "0") {
				format.push_back(field);
				continue;
			}

			const bool file_ok = item.size() >= 3 && (item[0] == '1' || item[0] == '2');
			if (!file_ok || item[1] != '.') return false;
			if (!parseInt(item.substr(2), number) || number < 1) return false;
			field.file = item[0] - '0';
			field.field = static_cast<std::size_t>(number);
			format.push_back(field);
		}

		return true;
	}

	static bool takeJoinValue(const std::vector<std::string>& args, std::size_t& i,
	                          const char* flag, std::string& value) {
		const std::string& arg = args[i];
		if (arg.size() > 2 && arg.compare(0, 2, flag) == 0) {
			value = arg.substr(2);
			return true;
		}

		if (arg != flag || i + 1 >= args.size()) return false;
		value = args[++i];
		return true;
	}

	static bool parseFieldNumber(const std::string& text, std::size_t& field) {
		int number = 0;
		if (!parseInt(text, number) || number < 1) return false;
		field = static_cast<std::size_t>(number);
		return true;
	}

	static bool parseFileNumber(const std::string& text, int& file) {
		return parseInt(text, file) && (file == 1 || file == 2);
	}

	static bool applyJoinFieldOption(const std::vector<std::string>& args, std::size_t& i,
	                                 JoinOptions& options, bool& valid) {
		std::string value;
		if (takeJoinValue(args, i, "-1", value)) {
			valid = parseFieldNumber(value, options.field1);
			return true;
		}

		if (takeJoinValue(args, i, "-2", value)) {
			valid = parseFieldNumber(value, options.field2);
			return true;
		}

		if (!takeJoinValue(args, i, "-j", value)) return false;
		valid = parseFieldNumber(value, options.field1);
		options.field2 = options.field1;
		return true;
	}

	static bool applyJoinUnpairedOption(const std::vector<std::string>& args, std::size_t& i,
	                                    JoinOptions& options, bool& valid) {
		std::string value;
		const bool only = args[i].compare(0, 2, "-v") == 0;
		if (!takeJoinValue(args, i, "-a", value) && !takeJoinValue(args, i, "-v", value)) {
			return false;
		}

		int file = 0;
		valid = parseFileNumber(value, file);
		if (valid) options.unpaired[file] = true;
		if (only) options.only_unpaired = true;
		return true;
	}

	static bool applyJoinValue(const std::vector<std::string>& args, std::size_t& i,
	                           JoinOptions& options, bool& valid) {
		std::string value;
		if (takeJoinValue(args, i, "-t", value)) {
			options.separator = value.empty() ? '\0' : value[0];
			return true;
		}

		if (takeJoinValue(args, i, "-e", value)) {
			options.empty = value;
			return true;
		}

		if (takeJoinValue(args, i, "-o", value)) {
			valid = parseOutputFields(value, options.format);
			return true;
		}

		if (applyJoinFieldOption(args, i, options, valid)) return true;
		return applyJoinUnpairedOption(args, i, options, valid);
	}

	static bool parseJoinArgs(const std::vector<std::string>& args, JoinOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			if (arg == "-i" || arg == "--ignore-case") {
				options.ignore_case = true;
				continue;
			}

			if (arg == "--nocheck-order" || arg == "--check-order") continue;
			if (arg.size() < 2 || arg[0] != '-') {
				options.files.push_back(arg);
				continue;
			}

			bool valid = true;
			if (!applyJoinValue(args, i, options, valid) || !valid) {
				perr("join", "invalid option: " + arg);
				return false;
			}
		}

		if (options.files.size() == 2) return true;
		perr("join", "usage: join [-i] [-t C] [-1 F] [-2 F] [-a N] [-v N] [-e S] [-o LIST] "
			"FILE1 FILE2");
		return false;
	}

	static bool readJoinFile(Executor& exec, const std::string& file, std::size_t field,
	                         const JoinOptions& options, std::vector<JoinLine>& out) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, file, lines)) {
			perr("join", file + ": " + std::strerror(errno));
			return false;
		}

		for (const std::string& line : lines) {
			JoinLine entry;
			entry.fields = splitJoinFields(line, options.separator);
			entry.key = field <= entry.fields.size() ? entry.fields[field - 1] : "";
			if (options.ignore_case) entry.key = lowerAscii(entry.key);
			out.push_back(entry);
		}

		return true;
	}

	static std::string joinFieldOr(const JoinLine* line, std::size_t field,
	                               const std::string& fill) {
		if (line == nullptr || field > line->fields.size()) return fill;
		return line->fields[field - 1];
	}

	static std::string formattedJoinLine(const JoinLine* left, const JoinLine* right,
	                                     const JoinOptions& options) {
		const JoinLine* keyed = left != nullptr ? left : right;
		const std::size_t key_field = left != nullptr ? options.field1 : options.field2;
		std::string out;
		bool first = true;
		for (const OutputField& field : options.format) {
			std::string text = options.empty;
			if (field.file == 0) text = joinFieldOr(keyed, key_field, options.empty);
			if (field.file == 1) text = joinFieldOr(left, field.field, options.empty);
			if (field.file == 2) text = joinFieldOr(right, field.field, options.empty);
			if (!first) out.push_back(options.separator == '\0' ? ' ' : options.separator);
			out += text;
			first = false;
		}

		return out;
	}

	static void appendOtherFields(std::string& out, const JoinLine* line, std::size_t key_field,
	                              char separator) {
		if (line == nullptr) return;
		for (std::size_t k = 0; k < line->fields.size(); ++k) {
			if (k + 1 == key_field) continue;
			out.push_back(separator == '\0' ? ' ' : separator);
			out += line->fields[k];
		}
	}

	static void printJoinLine(const JoinLine* left, const JoinLine* right,
	                          const JoinOptions& options) {
		if (!options.format.empty()) {
			std::printf("%s\n", formattedJoinLine(left, right, options).c_str());
			return;
		}

		const JoinLine* keyed = left != nullptr ? left : right;
		const std::size_t key_field = left != nullptr ? options.field1 : options.field2;
		std::string out = joinFieldOr(keyed, key_field, "");
		appendOtherFields(out, left, options.field1, options.separator);
		appendOtherFields(out, right, options.field2, options.separator);
		std::printf("%s\n", out.c_str());
	}

	static std::size_t groupEnd(const std::vector<JoinLine>& lines, std::size_t start) {
		std::size_t end = start + 1;
		while (end < lines.size() && lines[end].key == lines[start].key) ++end;
		return end;
	}

	static void printUnpaired(const std::vector<JoinLine>& lines, std::size_t from,
	                          std::size_t to, int file, const JoinOptions& options) {
		if (!options.unpaired[file]) return;
		for (std::size_t k = from; k < to; ++k) {
			if (file == 1) printJoinLine(&lines[k], nullptr, options);
			else           printJoinLine(nullptr, &lines[k], options);
		}
	}

	static void printPairs(const std::vector<JoinLine>& left, GroupRange left_group,
	                       const std::vector<JoinLine>& right, GroupRange right_group,
	                       const JoinOptions& options) {
		for (std::size_t a = left_group.begin; a < left_group.end; ++a) {
			for (std::size_t b = right_group.begin; b < right_group.end; ++b) {
				printJoinLine(&left[a], &right[b], options);
			}
		}
	}

	static void mergeJoin(const std::vector<JoinLine>& left, const std::vector<JoinLine>& right,
	                      const JoinOptions& options) {
		std::size_t i = 0;
		std::size_t j = 0;
		while (i < left.size() && j < right.size()) {
			const int order = left[i].key.compare(right[j].key);
			if (order < 0) {
				printUnpaired(left, i, i + 1, 1, options);
				++i;
				continue;
			}

			if (order > 0) {
				printUnpaired(right, j, j + 1, 2, options);
				++j;
				continue;
			}

			const GroupRange left_group = { i, groupEnd(left, i) };
			const GroupRange right_group = { j, groupEnd(right, j) };
			if (!options.only_unpaired) printPairs(left, left_group, right, right_group, options);
			i = left_group.end;
			j = right_group.end;
		}

		printUnpaired(left, i, left.size(), 1, options);
		printUnpaired(right, j, right.size(), 2, options);
	}

	static int builtin_join(Executor& exec, const std::vector<std::string>& args) {
		JoinOptions options;
		if (!parseJoinArgs(args, options)) return 1;

		std::vector<JoinLine> left;
		std::vector<JoinLine> right;
		if (!readJoinFile(exec, options.files[0], options.field1, options, left)) return 1;
		if (!readJoinFile(exec, options.files[1], options.field2, options, right)) return 1;

		mergeJoin(left, right, options);
		std::fflush(stdout);
		return 0;
	}

	static bool parseShufRange(const std::string& text, ShufOptions& options) {
		const std::size_t dash = text.find('-', 1);
		if (dash == std::string::npos) return false;

		options.have_range = parseLL(text.substr(0, dash), options.low)
			&& parseLL(text.substr(dash + 1), options.high) && options.low <= options.high;
		return options.have_range;
	}

	static bool applyShufOption(const std::vector<std::string>& args, std::size_t& i,
	                            ShufOptions& options) {
		const std::string& arg = args[i];
		std::string value;
		if (takeJoinValue(args, i, "-n", value)) return parseULL(value, options.count);
		if (takeJoinValue(args, i, "-i", value)) return parseShufRange(value, options);
		if (takeJoinValue(args, i, "-o", value)) {
			options.output = value;
			return true;
		}

		const bool zero = arg == "-z" || arg == "--zero-terminated";
		if (arg == "-e" || arg == "--echo")        options.echo_args = true;
		else if (arg == "-r" || arg == "--repeat") options.repeat = true;
		else if (zero)                             options.zero_terminated = true;
		else return false;
		return true;
	}

	static bool parseShufArgs(const std::vector<std::string>& args, ShufOptions& options) {
		for (std::size_t i = 0; i < args.size(); ++i) {
			const std::string& arg = args[i];
			const bool option = arg.size() > 1 && arg[0] == '-' && !options.echo_args;
			if (option && applyShufOption(args, i, options)) continue;
			if (option && arg != "-") {
				perr("shuf", "invalid option: " + arg);
				return false;
			}

			options.operands.push_back(arg);
		}

		return true;
	}

	static bool shufInputLines(Executor& exec, const ShufOptions& options,
	                           std::vector<std::string>& lines) {
		if (options.echo_args) {
			lines = options.operands;
			return true;
		}

		if (options.have_range) {
			for (long long value = options.low; value <= options.high; ++value) {
				lines.push_back(std::to_string(value));
			}

			return true;
		}

		const std::string file = options.operands.empty() ? "-" : options.operands[0];
		if (readAllLines(exec, file, lines)) return true;

		perr("shuf", file + ": " + std::strerror(errno));
		return false;
	}

	static bool writeShufLine(std::FILE* out, const std::string& line, char terminator) {
		if (std::fwrite(line.data(), 1, line.size(), out) != line.size()) return false;
		return std::fputc(terminator, out) != EOF;
	}

	static void emitShuffled(std::FILE* out, std::vector<std::string>& lines,
	                         const ShufOptions& options) {
		std::random_device seed;
		std::mt19937_64 generator((static_cast<unsigned long long>(seed()) << 32) ^ seed());
		const char terminator = options.zero_terminated ? '\0' : '\n';
		if (options.repeat) {
			if (lines.empty()) return;
			std::uniform_int_distribution<std::size_t> pick(0, lines.size() - 1);
			for (unsigned long long k = 0; k < options.count; ++k) {
				if (!writeShufLine(out, lines[pick(generator)], terminator)) return;
			}

			return;
		}

		std::shuffle(lines.begin(), lines.end(), generator);
		const std::size_t shown = static_cast<std::size_t>(
			(std::min)(static_cast<unsigned long long>(lines.size()), options.count));
		for (std::size_t k = 0; k < shown; ++k) {
			if (!writeShufLine(out, lines[k], terminator)) return;
		}
	}

	static int builtin_shuf(Executor& exec, const std::vector<std::string>& args) {
		ShufOptions options;
		if (!parseShufArgs(args, options)) return 1;

		std::vector<std::string> lines;
		if (!shufInputLines(exec, options, lines)) return 1;

		std::FILE* out = options.output.empty() ? stdout : fopenNative(exec, options.output, "wb");
		if (out == nullptr) {
			perr("shuf", options.output + ": " + std::strerror(errno));
			return 1;
		}

		emitShuffled(out, lines, options);
		if (out != stdout) std::fclose(out);
		std::fflush(stdout);
		return 0;
	}

	static std::size_t tsortNode(TsortGraph& graph, const std::string& name) {
		const auto found = graph.index.find(name);
		if (found != graph.index.end()) return found->second;

		const std::size_t id = graph.names.size();
		graph.index.emplace(name, id);
		graph.names.push_back(name);
		graph.edges.emplace_back();
		graph.indegree.push_back(0);
		return id;
	}

	static bool readTsortGraph(Executor& exec, const std::string& file, TsortGraph& graph) {
		std::vector<std::string> lines;
		if (!readAllLines(exec, file, lines)) {
			perr("tsort", file + ": " + std::strerror(errno));
			return false;
		}

		std::vector<std::string> tokens;
		for (const std::string& line : lines) {
			const std::vector<std::string> words = splitJoinFields(line, '\0');
			tokens.insert(tokens.end(), words.begin(), words.end());
		}

		if (tokens.size() % 2 != 0) {
			perr("tsort", file + ": input contains an odd number of tokens");
			return false;
		}

		for (std::size_t k = 0; k < tokens.size(); k += 2) {
			const std::size_t from = tsortNode(graph, tokens[k]);
			const std::size_t to = tsortNode(graph, tokens[k + 1]);
			if (from == to) continue;
			graph.edges[from].push_back(to);
			++graph.indegree[to];
		}

		return true;
	}

	static std::size_t firstRemaining(const TsortGraph& graph, const std::vector<bool>& done) {
		for (std::size_t k = 0; k < graph.names.size(); ++k) {
			if (!done[k]) return k;
		}

		return graph.names.size();
	}

	static void reportTsortLoop(const TsortGraph& graph, const std::vector<bool>& done,
	                            std::size_t start, const std::string& file) {
		std::fprintf(stderr, "tsort: %s: input contains a loop:\n", file.c_str());
		std::vector<bool> seen(graph.names.size(), false);
		for (std::size_t node = start; !seen[node];) {
			seen[node] = true;
			std::fprintf(stderr, "tsort: %s\n", graph.names[node].c_str());
			std::size_t next = node;
			for (const std::size_t to : graph.edges[node]) {
				if (done[to]) continue;
				next = to;
				break;
			}

			if (next == node) break;
			node = next;
		}
	}

	static void releaseNode(TsortGraph& graph, std::size_t node, std::vector<std::size_t>& ready) {
		for (const std::size_t to : graph.edges[node]) {
			if (--graph.indegree[to] == 0) ready.push_back(to);
		}
	}

	static bool printTopologicalOrder(TsortGraph& graph, const std::string& file) {
		std::vector<std::size_t> ready;
		for (std::size_t k = 0; k < graph.names.size(); ++k) {
			if (graph.indegree[k] == 0) ready.push_back(k);
		}

		std::vector<bool> done(graph.names.size(), false);
		bool acyclic = true;
		for (std::size_t emitted = 0; emitted < graph.names.size(); ++emitted) {
			if (ready.empty()) {
				const std::size_t stuck = firstRemaining(graph, done);
				reportTsortLoop(graph, done, stuck, file);
				acyclic = false;
				graph.indegree[stuck] = 0;
				ready.push_back(stuck);
			}

			const std::size_t node = ready.front();
			ready.erase(ready.begin());
			done[node] = true;
			std::printf("%s\n", graph.names[node].c_str());
			releaseNode(graph, node, ready);
		}

		return acyclic;
	}

	static int builtin_tsort(Executor& exec, const std::vector<std::string>& args) {
		const std::string file = args.empty() ? "-" : args[0];
		TsortGraph graph;
		if (!readTsortGraph(exec, file, graph)) return 1;

		const bool acyclic = printTopologicalOrder(graph, file);
		std::fflush(stdout);
		return acyclic ? 0 : 1;
	}

	static bool applyStringsOption(const std::vector<std::string>& args, std::size_t& i,
	                               StringsOptions& options) {
		const std::string& arg = args[i];
		unsigned long long length = 0;
		if (arg == "-n" && i + 1 < args.size()) {
			if (!parseULL(args[++i], length) || length == 0) return false;
			options.min_length = static_cast<std::size_t>(length);
			return true;
		}

		if (arg.size() > 1 && std::isdigit(static_cast<unsigned char>(arg[1])) != 0) {
			if (!parseULL(arg.substr(1), length) || length == 0) return false;
			options.min_length = static_cast<std::size_t>(length);
			return true;
		}

		if (arg == "-t" && i + 1 < args.size()) {
			options.radix = args[++i].empty() ? 'd' : args[i][0];
			return std::strchr("dox", options.radix) != nullptr;
		}

		if (arg == "-o") options.radix = 'o';
		else if (arg == "-f") options.show_name = true;
		else return arg == "-a" || arg == "--all";
		return true;
	}

	static void printStringRun(const std::string& run, unsigned long long offset,
	                           const StringsOptions& options, const std::string& name) {
		if (options.show_name) std::printf("%s: ", name.c_str());
		if (options.radix == 'o') std::printf("%7llo ", offset);
		if (options.radix == 'd') std::printf("%7llu ", offset);
		if (options.radix == 'x') std::printf("%7llx ", offset);
		std::printf("%s\n", run.c_str());
	}

	static bool isStringByte(int byte) {
		return (byte >= 32 && byte < 127) || byte == '\t';
	}

	static void scanStrings(std::FILE* stream, const StringsOptions& options,
	                        const std::string& name) {
		std::string run;
		unsigned long long offset = 0;
		unsigned long long run_start = 0;
		for (int byte = std::fgetc(stream); byte != EOF; byte = std::fgetc(stream), ++offset) {
			if (isStringByte(byte)) {
				if (run.empty()) run_start = offset;
				run.push_back(static_cast<char>(byte));
				continue;
			}

			if (run.size() >= options.min_length) printStringRun(run, run_start, options, name);
			run.clear();
			if (stdoutFailed()) return;
		}

		if (run.size() >= options.min_length) printStringRun(run, run_start, options, name);
	}

	static int builtin_strings(Executor& exec, const std::vector<std::string>& args) {
		StringsOptions options;
		for (std::size_t i = 0; i < args.size(); ++i) {
			if (args[i].size() < 2 || args[i][0] != '-') {
				options.files.push_back(args[i]);
				continue;
			}

			if (applyStringsOption(args, i, options)) continue;
			perr("strings", "invalid option: " + args[i]);
			return 1;
		}

		if (options.files.empty()) options.files.push_back("-");
		int status = 0;
		for (const std::string& file : options.files) {
			std::FILE* stream = file == "-" ? stdin : fopenNative(exec, file, "rb");
			if (stream == nullptr) {
				perr("strings", file + ": " + std::strerror(errno));
				status = 1;
				continue;
			}

			scanStrings(stream, options, file == "-" ? "{standard input}" : file);
			if (stream != stdin) std::fclose(stream);
		}

		std::fflush(stdout);
		return status;
	}

	void registerFilterBuiltins(Executor& exec) {
		exec.registerBuiltin("join",    builtin_join);
		exec.registerBuiltin("shuf",    builtin_shuf);
		exec.registerBuiltin("tsort",   builtin_tsort);
		exec.registerBuiltin("strings", builtin_strings);
	}

}  // namespace wbsh
