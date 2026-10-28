#pragma once

/**
 * @file bc_internal.h
 * @brief Syntax tree, runtime state and the entry points shared by the bc translation units.
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "bcnum.h"

namespace wbsh {

	namespace bc_detail {

		inline constexpr std::int64_t kMaxIbase = 16;
		inline constexpr std::int64_t kMaxObase = 999999999;
		inline constexpr std::int64_t kMaxDim = 16777215;
		inline constexpr std::int64_t kMaxScale = 2147483647;
		inline constexpr int kDefaultLineLength = 70;
		inline constexpr int kMathLibScale = 20;

		enum class ExprKind {
			Number, Variable, Element, Scale, Ibase, Obase, Last,
			Assign, PreIncrement, PreDecrement, PostIncrement, PostDecrement,
			Negate, Not, Binary, Relation, And, Or,
			Call, Length, Sqrt, ScaleOf, Read,
		};

		enum class BinaryOp { None, Add, Sub, Mul, Div, Mod, Pow };
		enum class RelOp { Eq, Ne, Lt, Le, Gt, Ge };

		struct Expr;
		using ExprPtr = std::unique_ptr<Expr>;

		/// A call argument is either an expression or a whole array (`a[]`).
		struct CallArg {
			ExprPtr value;
			int array_id = -1;
		};

		struct Expr {
			ExprKind kind = ExprKind::Number;
			BinaryOp op = BinaryOp::None;
			RelOp relation = RelOp::Eq;
			int name_id = -1;
			std::string literal;
			ExprPtr left;
			ExprPtr right;
			std::vector<CallArg> args;
			bool parenthesized = false;

			// Literals convert with the ibase in effect when they run.
			int cached_base = 0;
			BcNum cached;
		};

		enum class StmtKind {
			Empty, Expression, String, Print, Block, If, While, For,
			Break, Continue, Return, Halt, Limits, Warranty,
		};

		struct Stmt;
		using StmtPtr = std::unique_ptr<Stmt>;
		using StmtList = std::vector<StmtPtr>;

		struct PrintItem {
			ExprPtr value;
			std::string text;
		};

		struct Stmt {
			StmtKind kind = StmtKind::Empty;
			ExprPtr expr;
			ExprPtr init;
			ExprPtr step;
			StmtPtr body;
			StmtPtr else_body;
			StmtList list;
			std::string text;
			std::vector<PrintItem> items;
		};

		struct Param {
			int id = -1;
			bool is_array = false;
			bool by_ref = false;
		};

		struct Function {
			int id = -1;
			bool is_void = false;
			std::vector<Param> params;
			std::vector<Param> autos;
			StmtList body;
		};

		struct BcArray {
			std::vector<BcNum> items;
		};

		struct BcOutput {
			int line_length = kDefaultLineLength;
			int column = 0;
			std::string pending;

			void put(char c);
			void write(const std::string& text);
			void flush();
		};

		/// Where program text comes from: a file, standard input or a string.
		struct BcSource {
			std::FILE* file = nullptr;
			std::string text;
			std::size_t text_pos = 0;
			std::string name;

			bool readLine(std::string& line);
		};

		struct BcState {
			std::unordered_map<std::string, int> name_ids;
			std::vector<std::string> names;
			std::vector<std::shared_ptr<Function>> functions;
			std::vector<std::vector<BcNum>> scalars;
			std::vector<std::vector<std::shared_ptr<BcArray>>> arrays;

			std::int64_t scale = 0;
			int ibase = 10;
			std::int64_t obase = 10;
			BcNum last;

			bool mathlib = false;
			bool halted = false;
			bool had_error = false;
			int call_depth = 0;
			std::vector<int> call_stack;
			BcNum return_value;

			BcOutput out;

			int intern(const std::string& name);
		};

		void reportError(BcState& state, const std::string& message);

		enum class ParseStatus { Item, Defined, End, Error, Quit };

		struct ParserImpl;

		class BcParser {
		public:
			BcParser(BcState& state, BcSource& source);
			~BcParser();

			/// Parses the next top-level item: one line of statements or a define.
			ParseStatus next(StmtList& item);

		private:
			std::unique_ptr<ParserImpl> impl_;
		};

		bool parseNumberText(BcState& state, const std::string& text, ExprPtr& out);

		/// Runs one parsed item; false once bc must stop (halt).
		bool runItem(BcState& state, StmtList& item);

		void writeNumber(BcOutput& out, const BcNum& value, std::int64_t obase);

		bool isMathLibFunction(const std::string& name);
		bool callMathLib(const std::string& name, const std::vector<BcNum>& args,
			std::int64_t scale, BcNum& out);

	}  // namespace bc_detail

}  // namespace wbsh
