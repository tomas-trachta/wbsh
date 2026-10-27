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
		static bool gsubStep(GsubScan& scan, std::size_t start, std::size_t length,
				const std::string& replacement) {
			const bool at_end = start >= scan.subject.size();
			if (length == 0 && start == scan.last_match_end) {
				if (at_end) return false;
				copyOneCharThrough(scan, start);
				return true;
			}

			scan.out.append(scan.subject, scan.pos, start - scan.pos);
			appendReplacement(scan.out, replacement, scan.subject.substr(start, length));
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

		static int gsubAll(std::string& subject, const AwkRegex& re,
				const std::string& replacement) {
			GsubScan scan{ subject };
			std::size_t start = 0;
			std::size_t length = 0;
			while (awkRegexFind(re, subject, scan.pos, start, length)) {
				if (!gsubStep(scan, start, length, replacement)) break;
			}

			if (scan.pos < subject.size()) scan.out.append(subject, scan.pos, std::string::npos);
			subject = std::move(scan.out);
			return scan.count;
		}

		static int subFirst(std::string& subject, const AwkRegex& re,
				const std::string& replacement) {
			std::size_t start = 0;
			std::size_t length = 0;
			if (!awkRegexFind(re, subject, 0, start, length)) return 0;

			std::string out = subject.substr(0, start);
			appendReplacement(out, replacement, subject.substr(start, length));
			out.append(subject, start + length, std::string::npos);
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

		std::shared_ptr<const AwkRegex> Interpreter::regexArg(Expr& call, std::size_t index) {
			if (index >= call.args.size()) return dynamicRegex("");
			return regexOperand(*call.args[index]);
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
			const bool lower = call.builtin == Builtin::Tolower;
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

			const AwkRegex* re = literalRegex(separator);
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
			const std::shared_ptr<const AwkRegex> re = regexArg(call, 0);
			const std::string replacement = argText(call, 1);
			Expr* target = (call.args.size() >= 3) ? call.args[2] : nullptr;
			std::string subject = target != nullptr ? text(eval(*target)) : record;
			if (!re->ok) return AwkValue::num(0);

			const bool global = call.builtin == Builtin::Gsub;
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
			const std::shared_ptr<const AwkRegex> re = regexArg(call, 1);

			std::size_t found = 0;
			std::size_t length = 0;
			if (!awkRegexFind(*re, subject, 0, found, length)) {
				setGlobal(kRSTART, AwkValue::num(0));
				setGlobal(kRLENGTH, AwkValue::num(-1));
				return AwkValue::num(0);
			}

			const auto start = static_cast<double>(found + 1);
			setGlobal(kRSTART, AwkValue::num(start));
			setGlobal(kRLENGTH, AwkValue::num(static_cast<double>(length)));
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
			if (call.builtin == Builtin::Rand) {
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
			const double x = argNumber(call, 0);
			switch (call.builtin) {
				case Builtin::Int:   return AwkValue::num(std::trunc(x));
				case Builtin::Sqrt:  return AwkValue::num(std::sqrt(x));
				case Builtin::Exp:   return AwkValue::num(std::exp(x));
				case Builtin::Log:   return AwkValue::num(std::log(x));
				case Builtin::Sin:   return AwkValue::num(std::sin(x));
				case Builtin::Cos:   return AwkValue::num(std::cos(x));
				case Builtin::Atan2: return AwkValue::num(std::atan2(x, argNumber(call, 1)));
				default:             return AwkValue::num(0);
			}
		}

		AwkValue Interpreter::callBuiltin(Expr& call) {
			switch (call.builtin) {
				case Builtin::Length:  return callLength(call);
				case Builtin::Substr:  return callSubstr(call);
				case Builtin::Index:   return callIndex(call);
				case Builtin::Tolower:
				case Builtin::Toupper: return callCaseConvert(call);
				case Builtin::Split:   return callSplit(call);
				case Builtin::Sub:
				case Builtin::Gsub:    return callSubOrGsub(call);
				case Builtin::Match:   return callMatch(call);
				case Builtin::Sprintf: return callSprintf(call);
				case Builtin::System:  return callSystem(call);
				case Builtin::Close:   return callClose(call);
				case Builtin::Fflush:  return callFflush(call);
				case Builtin::Rand:
				case Builtin::Srand:   return callRandom(call);
				default:               return callMath(call);
			}
		}

	}  // namespace awk_detail

}  // namespace wbsh
