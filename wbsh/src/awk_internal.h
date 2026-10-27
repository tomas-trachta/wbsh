#pragma once

/**
 * @file awk_internal.h
 * @brief Values, syntax tree and the parser entry point shared by the awk translation units.
 */

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "awk_regex.h"

namespace wbsh {

	namespace awk_detail {

		inline constexpr char kDefaultNumberFormat[] = "%.6g";
		inline constexpr int kUsageError = 2;

		bool looksNumeric(const std::string& text, double& value);
		double stringToNumber(const std::string& text);
		double parseAwkNumber(const std::string& number_text);
		std::string numberToString(double number, const std::string& format);

		/// The escapes of an awk string literal (\n, \t, \ooo, ...); unknown
		/// escapes keep their backslash.
		std::string expandEscapes(const std::string& text);

		// A value with neither flag is uninitialised: "" and 0 at once. Input
		// text that looks numeric (a POSIX "strnum") has both flags and keeps
		// its original spelling as the string.
		struct AwkValue {
			double number = 0.0;
			std::string text;
			bool is_number = false;
			bool is_string = false;
			// A freshly split field: whether it is a strnum is decided on first use.
			bool strnum_pending = false;

			AwkValue() = default;

			static AwkValue num(double value) {
				AwkValue result;
				result.number = value;
				result.is_number = true;
				return result;
			}

			static AwkValue str(std::string value) {
				AwkValue result;
				result.text = std::move(value);
				result.is_string = true;
				return result;
			}

			static AwkValue input(std::string value) {
				AwkValue result;
				result.is_number = looksNumeric(value, result.number);
				result.text = std::move(value);
				result.is_string = true;
				return result;
			}

			static AwkValue pendingInput(std::string value) {
				AwkValue result = str(std::move(value));
				result.strnum_pending = true;
				return result;
			}

			void resolvePending() {
				if (!strnum_pending) return;

				strnum_pending = false;
				is_number = looksNumeric(text, number);
			}

			bool isUninitialised() const {
				return !is_number && !is_string;
			}

			bool comparesAsNumber() const {
				return is_number || !is_string;
			}

			double asNumber() const {
				if (is_number) return number;
				if (!is_string) return 0.0;
				return stringToNumber(text);
			}

			std::string asString(const std::string& format = kDefaultNumberFormat) const {
				if (is_string) return text;
				if (!is_number) return std::string();
				return numberToString(number, format);
			}

			bool truthy() const {
				if (is_number) return number != 0.0;
				return !text.empty();
			}
		};

		inline AwkValue boolValue(bool value) {
			return AwkValue::num(value ? 1.0 : 0.0);
		}

		std::string formatPrintf(const std::vector<AwkValue>& args, const std::string& convfmt);

		using AwkArray = std::map<std::string, AwkValue>;

		// Fixed global slots of the special variables, in this order.
		enum SpecialVar : int {
			kNF, kNR, kFNR, kFS, kOFS, kORS, kRS, kFILENAME, kSUBSEP, kCONVFMT, kOFMT,
			kRSTART, kRLENGTH, kENVIRON, kARGC, kARGV,
			kSpecialCount,
		};

		const char* specialVarName(int slot);

		struct Stmt;
		struct FunctionDef;

		enum class ExprKind {
			Number, String, Regex, Var,
			Field,
			ArrayRef,
			ArrayInTest,
			Unary,
			PostIncDec,
			Binary,
			Ternary,
			Assign,
			Call,
			UserCall,
			Getline,
			Group,
		};

		enum class GetlineSource { Main, File, Command };

		enum class Op {
			None,
			Add, Sub, Mul, Div, Mod, Pow, Concat,
			Lt, Le, Gt, Ge, Eq, Ne,
			And, Or, Match, NoMatch,
			Not, Negate, UnaryPlus, PreIncrement, PreDecrement, PostIncrement, PostDecrement,
			Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, PowAssign,
		};

		enum class Builtin {
			None,
			Length, Substr, Index, Split, Sub, Gsub, Match, Sprintf,
			Sin, Cos, Atan2, Exp, Log, Sqrt, Int, Rand, Srand,
			Tolower, Toupper, System, Close, Fflush,
		};

		struct Expr {
			ExprKind kind;
			double number = 0;
			std::string text;
			std::string name;
			Op op = Op::None;
			Builtin builtin = Builtin::None;
			Expr* first = nullptr;
			Expr* second = nullptr;
			Expr* third = nullptr;
			std::vector<Expr*> args;
			// Var / ArrayRef / ArrayInTest: the variable's slot in the globals
			// or, when `local` is set, in the running function's frame.
			int slot = -1;
			bool local = false;
			GetlineSource source = GetlineSource::Main;
			FunctionDef* function = nullptr;
			// A regex literal, compiled on first use.
			mutable std::shared_ptr<const AwkRegex> compiled;
		};

		enum class StmtKind {
			Empty, Print, Printf, ExprStmt, Block, If, While, DoWhile, For, ForIn,
			Break, Continue, Next, Exit, Delete, Return,
		};

		enum class Redirect { None, Truncate, Append, Pipe };

		struct Stmt {
			StmtKind kind;
			std::vector<Stmt*> children;
			std::vector<Expr*> exprs;
			Expr* init = nullptr;
			Expr* cond = nullptr;
			Expr* step = nullptr;
			Expr* redirect_target = nullptr;
			Redirect redirect = Redirect::None;
		};

		struct FunctionDef {
			std::string name;
			std::vector<std::string> params;
			Stmt* body = nullptr;
		};

		struct AwkPattern {
			enum Kind { Begin, End, Always, Condition, Range };
			Kind kind = Always;
			Expr* first = nullptr;
			Expr* second = nullptr;
		};

		struct AwkRule {
			AwkPattern pattern;
			Stmt* action = nullptr;
			bool in_range = false;
		};

		struct AwkProgram {
			std::vector<AwkRule> rules;
			std::unordered_map<std::string, FunctionDef*> functions;
			std::vector<std::string> global_names;
			Arena arena;
		};

		/// Parses @p text into @p program; false (with a message in @p error) on a syntax error.
		bool parseAwkProgram(const std::string& text, AwkProgram& program, std::string& error);

		Builtin builtinFunction(const std::string& name);
		bool isBuiltinFunction(const std::string& name);

	}  // namespace awk_detail

}  // namespace wbsh
