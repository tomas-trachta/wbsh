/**
 * @file awk_builtins.cpp
 * @brief awk builtin functions: string functions, sub/gsub, split, match, math, I/O helpers.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "awk_interp.h"
#include "regexutil.h"

namespace wbsh {

	namespace awk_detail {

		static bool startsWith(const std::string& text, std::size_t pos, const char* prefix) {
			return text.compare(pos, std::strlen(prefix), prefix) == 0;
		}

		// gawk's sub/gsub rules: "\\\&" is a literal "\&", "\\&" a backslash
		// followed by the match, "\&" a literal "&"; any other backslash is literal.
		static void appendReplacement(std::string& out, const std::string& replacement,
				const std::string& matched) {
			std::size_t i = 0;
			while (i < replacement.size()) {
				if (startsWith(replacement, i, "\\\\\\&")) {
					out += "\\&";
					i += 4;
				} else if (startsWith(replacement, i, "\\\\&")) {
					out += '\\';
					out += matched;
					i += 3;
				} else if (startsWith(replacement, i, "\\&")) {
					out += '&';
					i += 2;
				} else if (replacement[i] == '&') {
					out += matched;
					++i;
				} else {
					out += replacement[i];
					++i;
				}
			}
		}

		static bool searchFrom(const std::string& subject, std::size_t pos, const std::regex& re,
				std::smatch& match) {
			const auto flags = pos == 0
				? std::regex_constants::match_default
				: std::regex_constants::match_prev_avail;
			return searchRegexFlags(subject.cbegin() + static_cast<std::ptrdiff_t>(pos),
				subject.cend(), re, match, flags);
		}

		struct GsubScan {
			const std::string& subject;
			std::string out;
			std::size_t pos = 0;
			std::size_t last_match_end = std::string::npos;
			int count = 0;
		};

		static void copyOneCharThrough(GsubScan& scan, std::size_t at) {
			scan.out.append(scan.subject, scan.pos, at - scan.pos + 1);
			scan.pos = at + 1;
		}

		// Returns false once the scan has reached the end of the subject.
		static bool gsubStep(GsubScan& scan, const std::smatch& match,
				const std::string& replacement) {
			const std::size_t start = scan.pos + static_cast<std::size_t>(match.position(0));
			const auto length = static_cast<std::size_t>(match.length(0));
			const bool at_end = start >= scan.subject.size();
			if (length == 0 && start == scan.last_match_end) {
				if (at_end) return false;
				copyOneCharThrough(scan, start);
				return true;
			}

			scan.out.append(scan.subject, scan.pos, start - scan.pos);
			appendReplacement(scan.out, replacement, match.str(0));
			++scan.count;
			scan.pos = start + length;
			if (length != 0) {
				scan.last_match_end = scan.pos;
				return true;
			}

			if (at_end) return false;
			copyOneCharThrough(scan, start);
			return true;
		}

		static int gsubAll(std::string& subject, const std::regex& re,
				const std::string& replacement) {
			GsubScan scan{ subject };
			while (scan.pos <= subject.size()) {
				std::smatch match;
				if (!searchFrom(subject, scan.pos, re, match)) break;
				if (!gsubStep(scan, match, replacement)) break;
			}

			if (scan.pos < subject.size()) scan.out.append(subject, scan.pos, std::string::npos);
			subject = std::move(scan.out);
			return scan.count;
		}

		static int subFirst(std::string& subject, const std::regex& re,
				const std::string& replacement) {
			std::smatch match;
			if (!searchRegex(subject, re, &match)) return 0;

			std::string out = match.prefix().str();
			appendReplacement(out, replacement, match.str(0));
			out += match.suffix().str();
			subject = std::move(out);
			return 1;
		}

		std::string Interpreter::argText(Expr& call, std::size_t index) {
			if (index >= call.args.size()) return std::string();
			return text(eval(*call.args[index]));
		}

		double Interpreter::argNumber(Expr& call, std::size_t index) {
			if (index >= call.args.size()) return 0.0;
			return eval(*call.args[index]).asNumber();
		}

		// A regex literal argument is the regex itself, not a match against $0.
		const std::regex* Interpreter::regexArg(Expr& call, std::size_t index,
				std::regex& storage) {
			if (index >= call.args.size()) return dynamicRegex("", storage);

			Expr& arg = *call.args[index];
			if (arg.kind == ExprKind::Regex) return literalRegex(arg);
			return dynamicRegex(text(eval(arg)), storage);
		}

		AwkValue Interpreter::callLength(Expr& call) {
			if (call.args.empty()) return AwkValue::num(static_cast<double>(record.size()));

			Expr& arg = *call.args[0];
			if (arg.kind == ExprKind::Var) {
				const Cell& cell = cellFor(arg);
				if (cell.array && cell.value.isUninitialised()) {
					return AwkValue::num(static_cast<double>(cell.array->size()));
				}
			}

			return AwkValue::num(static_cast<double>(argText(call, 0).size()));
		}

		AwkValue Interpreter::callSubstr(Expr& call) {
			const std::string source = argText(call, 0);
			const auto source_size = static_cast<long long>(source.size());

			long long start = static_cast<long long>(argNumber(call, 1));
			long long length = (call.args.size() >= 3)
				? static_cast<long long>(argNumber(call, 2))
				: source_size - start + 1;
			if (start < 1) start = 1;

			if (start > source_size || length <= 0) return AwkValue::str("");

			const long long available = source_size - start + 1;
			return AwkValue::str(source.substr(static_cast<std::size_t>(start - 1),
				static_cast<std::size_t>(std::min<long long>(length, available))));
		}

		AwkValue Interpreter::callIndex(Expr& call) {
			const std::string haystack = argText(call, 0);
			const std::string needle = argText(call, 1);
			const std::size_t found = haystack.find(needle);
			if (found == std::string::npos) return AwkValue::num(0);
			return AwkValue::num(static_cast<double>(found + 1));
		}

		AwkValue Interpreter::callCaseConvert(Expr& call) {
			const bool lower = call.name == "tolower";
			std::string converted = argText(call, 0);
			for (auto& c : converted) {
				const auto raw = static_cast<unsigned char>(c);
				c = static_cast<char>(lower ? std::tolower(raw) : std::toupper(raw));
			}

			return AwkValue::str(std::move(converted));
		}

		std::vector<std::string> Interpreter::splitArgument(Expr& call, const std::string& source) {
			if (call.args.size() < 3) return splitWithSeparator(source, FS);

			Expr& separator = *call.args[2];
			if (separator.kind != ExprKind::Regex) {
				return splitWithSeparator(source, text(eval(separator)));
			}

			if (source.empty()) return {};

			const std::regex* re = literalRegex(separator);
			if (re == nullptr) return { source };
			return splitByRegex(source, *re);
		}

		AwkValue Interpreter::callSplit(Expr& call) {
			if (call.args.size() < 2 || call.args[1]->kind != ExprKind::Var) {
				runtimeError("split: the second argument must be an array");
				return AwkValue::num(0);
			}

			const std::string source = argText(call, 0);
			std::vector<std::string> parts = splitArgument(call, source);
			AwkArray& array = arrayFor(*call.args[1]);
			array.clear();
			for (std::size_t i = 0; i < parts.size(); ++i) {
				array[std::to_string(i + 1)] = AwkValue::input(std::move(parts[i]));
			}

			return AwkValue::num(static_cast<double>(parts.size()));
		}

		AwkValue Interpreter::callSubOrGsub(Expr& call) {
			std::regex storage;
			const std::regex* re = regexArg(call, 0, storage);
			const std::string replacement = argText(call, 1);
			Expr* target = (call.args.size() >= 3) ? call.args[2] : nullptr;
			std::string subject = target != nullptr ? text(eval(*target)) : record;
			if (re == nullptr) return AwkValue::num(0);

			const bool global = call.name == "gsub";
			const int count = global
				? gsubAll(subject, *re, replacement)
				: subFirst(subject, *re, replacement);
			if (count == 0) return AwkValue::num(0);

			if (target != nullptr) {
				assignLValue(*target, AwkValue::str(std::move(subject)));
			} else {
				setRecord(std::move(subject));
			}

			return AwkValue::num(static_cast<double>(count));
		}

		AwkValue Interpreter::callMatch(Expr& call) {
			const std::string subject = argText(call, 0);
			std::regex storage;
			const std::regex* re = regexArg(call, 1, storage);

			std::smatch match;
			if (re == nullptr || !searchRegex(subject, *re, &match)) {
				setGlobal(kRSTART, AwkValue::num(0));
				setGlobal(kRLENGTH, AwkValue::num(-1));
				return AwkValue::num(0);
			}

			const auto start = static_cast<double>(match.position(0) + 1);
			setGlobal(kRSTART, AwkValue::num(start));
			setGlobal(kRLENGTH, AwkValue::num(static_cast<double>(match.length(0))));
			return AwkValue::num(start);
		}

		AwkValue Interpreter::callSprintf(Expr& call) {
			std::vector<AwkValue> values;
			values.reserve(call.args.size());
			for (auto& arg : call.args) values.push_back(eval(*arg));
			return AwkValue::str(formatPrintf(values, CONVFMT));
		}

		AwkValue Interpreter::callSystem(Expr& call) {
			const std::string command = argText(call, 0);
			flushAllOutput();
			return AwkValue::num(static_cast<double>(runShellCommand(command)));
		}

		AwkValue Interpreter::callClose(Expr& call) {
			return AwkValue::num(static_cast<double>(closeStream(argText(call, 0))));
		}

		AwkValue Interpreter::callFflush(Expr& call) {
			if (call.args.empty()) {
				flushAllOutput();
				return AwkValue::num(0);
			}

			const std::string name = argText(call, 0);
			if (name.empty()) {
				flushAllOutput();
				return AwkValue::num(0);
			}

			const auto it = outputs.find(name);
			if (it == outputs.end()) return AwkValue::num(-1);
			if (it->second->file != nullptr) std::fflush(it->second->file);
			return AwkValue::num(0);
		}

		AwkValue Interpreter::callRandom(Expr& call) {
			if (call.name == "rand") {
				const double range = static_cast<double>(RAND_MAX) + 1.0;
				return AwkValue::num(static_cast<double>(std::rand()) / range);
			}

			const unsigned previous = random_seed;
			random_seed = call.args.empty()
				? static_cast<unsigned>(std::time(nullptr))
				: static_cast<unsigned>(argNumber(call, 0));
			std::srand(random_seed);
			return AwkValue::num(static_cast<double>(previous));
		}

		AwkValue Interpreter::callMath(Expr& call) {
			const std::string& name = call.name;
			if (name == "int")   return AwkValue::num(std::trunc(argNumber(call, 0)));
			if (name == "sqrt")  return AwkValue::num(std::sqrt(argNumber(call, 0)));
			if (name == "exp")   return AwkValue::num(std::exp(argNumber(call, 0)));
			if (name == "log")   return AwkValue::num(std::log(argNumber(call, 0)));
			if (name == "sin")   return AwkValue::num(std::sin(argNumber(call, 0)));
			if (name == "cos")   return AwkValue::num(std::cos(argNumber(call, 0)));
			if (name == "atan2") {
				const double y = argNumber(call, 0);
				return AwkValue::num(std::atan2(y, argNumber(call, 1)));
			}

			return callRandom(call);
		}

		AwkValue Interpreter::callBuiltin(Expr& call) {
			const std::string& name = call.name;
			if (name == "length")                        return callLength(call);
			if (name == "substr")                        return callSubstr(call);
			if (name == "index")                         return callIndex(call);
			if (name == "tolower" || name == "toupper")  return callCaseConvert(call);
			if (name == "split")                         return callSplit(call);
			if (name == "sub" || name == "gsub")         return callSubOrGsub(call);
			if (name == "match")                         return callMatch(call);
			if (name == "sprintf")                       return callSprintf(call);
			if (name == "system")                        return callSystem(call);
			if (name == "close")                         return callClose(call);
			if (name == "fflush")                        return callFflush(call);
			return callMath(call);
		}

	}  // namespace awk_detail

}  // namespace wbsh
