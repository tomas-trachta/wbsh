#include "bc_internal.h"

#include <cstdio>
#include <utility>

namespace wbsh {

	namespace bc_detail {

		static const int kMaxCallDepth = 30000;

		enum class Flow { Normal, Break, Continue, Return, Abort };

		struct LValue {
			ExprKind kind = ExprKind::Variable;
			int id = -1;
			std::int64_t index = 0;
		};

		int BcState::intern(const std::string& name) {
			const auto found = name_ids.find(name);
			if (found != name_ids.end()) return found->second;

			const int id = static_cast<int>(names.size());
			names.push_back(name);
			name_ids.emplace(name, id);
			scalars.resize(names.size());
			arrays.resize(names.size());
			return id;
		}

		void reportError(BcState& state, const std::string& message) {
			state.out.flush();
			state.had_error = true;
			std::fprintf(stderr, "%s\n", message.c_str());
			std::fflush(stderr);
		}

		static std::string currentFunctionName(const BcState& state) {
			if (state.call_stack.empty()) return "(main)";
			return state.names[static_cast<std::size_t>(state.call_stack.back())];
		}

		static bool runtimeError(BcState& state, const std::string& message) {
			reportError(state, "Runtime error (func=" + currentFunctionName(state) + "): " + message);
			return false;
		}

		static void runtimeWarning(BcState& state, const std::string& message) {
			reportError(state, "Runtime warning (func=" + currentFunctionName(state) + "): " + message);
		}

		static bool eval(BcState& state, Expr& expr, BcNum& out);
		static Flow exec(BcState& state, Stmt& stmt);

		static std::vector<BcNum>& scalarStack(BcState& state, int id) {
			return state.scalars[static_cast<std::size_t>(id)];
		}

		static std::shared_ptr<BcArray>& currentArray(BcState& state, int id) {
			auto& stack = state.arrays[static_cast<std::size_t>(id)];
			if (stack.empty()) stack.push_back(std::make_shared<BcArray>());
			return stack.back();
		}

		static const std::string& nameOf(const BcState& state, int id) {
			return state.names[static_cast<std::size_t>(id)];
		}

		static bool evalNumber(BcState& state, Expr& expr, BcNum& out) {
			if (expr.cached_base != state.ibase) {
				expr.cached = bcFromLiteral(expr.literal, state.ibase);
				expr.cached_base = state.ibase;
			}

			out = expr.cached;
			return true;
		}

		static bool resolveIndex(BcState& state, Expr& index_expr, int id, std::int64_t& index) {
			BcNum value;
			if (!eval(state, index_expr, value)) return false;
			if (bcToInt64(value, index) && index >= 0 && index <= kMaxDim) return true;
			return runtimeError(state, "Array " + nameOf(state, id) + " subscript out of bounds.");
		}

		static bool resolve(BcState& state, Expr& target, LValue& lvalue) {
			lvalue.kind = target.kind;
			lvalue.id = target.name_id;
			if (target.kind != ExprKind::Element) return true;
			return resolveIndex(state, *target.left, target.name_id, lvalue.index);
		}

		static BcNum readElement(BcState& state, const LValue& lvalue) {
			const auto& items = currentArray(state, lvalue.id)->items;
			const auto index = static_cast<std::size_t>(lvalue.index);
			return index < items.size() ? items[index] : BcNum();
		}

		static BcNum readLValue(BcState& state, const LValue& lvalue) {
			switch (lvalue.kind) {
				case ExprKind::Variable: {
					const auto& stack = scalarStack(state, lvalue.id);
					return stack.empty() ? BcNum() : stack.back();
				}
				case ExprKind::Element:
					return readElement(state, lvalue);
				case ExprKind::Scale:
					return bcFromInt(state.scale);
				case ExprKind::Ibase:
					return bcFromInt(state.ibase);
				case ExprKind::Obase:
					return bcFromInt(state.obase);
				default:
					return state.last;
			}
		}

		static bool setScale(BcState& state, const BcNum& value) {
			std::int64_t scale = 0;
			if (value.negative) return runtimeError(state, "negative scale");
			if (!bcToInt64(value, scale) || scale > kMaxScale) return runtimeError(state, "scale too large");

			state.scale = scale;
			return true;
		}

		static std::int64_t clampBase(BcState& state, const BcNum& value, const char* name, std::int64_t top) {
			std::int64_t base = 0;
			const bool fits = bcToInt64(value, base);
			if (fits && base >= 2 && base <= top) return base;

			const bool small = value.negative || (fits && base < 2);
			const std::int64_t clamped = small ? 2 : top;
			runtimeWarning(state, std::string(name) + (small ? " too small" : " too large")
				+ ", set to " + std::to_string(clamped));
			return clamped;
		}

		static void writeElement(BcState& state, const LValue& lvalue, const BcNum& value) {
			auto& items = currentArray(state, lvalue.id)->items;
			const auto index = static_cast<std::size_t>(lvalue.index);
			if (index >= items.size()) items.resize(index + 1);
			items[index] = value;
		}

		static bool writeLValue(BcState& state, const LValue& lvalue, const BcNum& value) {
			switch (lvalue.kind) {
				case ExprKind::Variable: {
					auto& stack = scalarStack(state, lvalue.id);
					if (stack.empty()) stack.push_back(value);
					else stack.back() = value;
					return true;
				}
				case ExprKind::Element:
					writeElement(state, lvalue, value);
					return true;
				case ExprKind::Scale:
					return setScale(state, value);
				case ExprKind::Ibase:
					state.ibase = static_cast<int>(clampBase(state, value, "ibase", kMaxIbase));
					return true;
				case ExprKind::Obase:
					state.obase = clampBase(state, value, "obase", kMaxObase);
					return true;
				default:
					state.last = value;
					return true;
			}
		}

		static bool applyPower(BcState& state, const BcNum& base, const BcNum& exponent, BcNum& out) {
			if (bcCompare(exponent, bcIntegerPart(exponent)) != 0) runtimeWarning(state, "non-zero scale in exponent");

			switch (bcPow(base, exponent, state.scale, out)) {
				case PowResult::Ok:
					return true;
				case PowResult::DivideByZero:
					return runtimeError(state, "Divide by zero");
				default:
					return runtimeError(state, "exponent too large in raise");
			}
		}

		static bool applyBinary(BcState& state, BinaryOp op, const BcNum& left, const BcNum& right, BcNum& out) {
			switch (op) {
				case BinaryOp::Add:
					out = bcAdd(left, right);
					return true;
				case BinaryOp::Sub:
					out = bcSub(left, right);
					return true;
				case BinaryOp::Mul:
					out = bcMul(left, right, state.scale);
					return true;
				case BinaryOp::Div:
					return bcDiv(left, right, state.scale, out) || runtimeError(state, "Divide by zero");
				case BinaryOp::Mod:
					return bcMod(left, right, state.scale, out) || runtimeError(state, "Modulo by zero");
				case BinaryOp::Pow:
					return applyPower(state, left, right, out);
				default:
					out = right;
					return true;
			}
		}

		static bool compareRelation(RelOp relation, int cmp) {
			switch (relation) {
				case RelOp::Eq:
					return cmp == 0;
				case RelOp::Ne:
					return cmp != 0;
				case RelOp::Lt:
					return cmp < 0;
				case RelOp::Le:
					return cmp <= 0;
				case RelOp::Gt:
					return cmp > 0;
				default:
					return cmp >= 0;
			}
		}

		static bool evalAssign(BcState& state, Expr& expr, BcNum& out) {
			LValue lvalue;
			if (!resolve(state, *expr.left, lvalue)) return false;

			BcNum value;
			if (!eval(state, *expr.right, value)) return false;
			if (expr.op != BinaryOp::None && !applyBinary(state, expr.op, readLValue(state, lvalue), value, value)) {
				return false;
			}

			if (!writeLValue(state, lvalue, value)) return false;
			out = std::move(value);
			return true;
		}

		static bool evalStep(BcState& state, Expr& expr, int delta, bool prefix, BcNum& out) {
			LValue lvalue;
			if (!resolve(state, *expr.left, lvalue)) return false;

			const BcNum current = readLValue(state, lvalue);
			const BcNum updated = bcAdd(current, bcFromInt(delta));
			if (!writeLValue(state, lvalue, updated)) return false;

			out = prefix ? updated : current;
			return true;
		}

		static bool evalBinary(BcState& state, Expr& expr, BcNum& out) {
			BcNum left;
			BcNum right;
			if (!eval(state, *expr.left, left) || !eval(state, *expr.right, right)) return false;
			return applyBinary(state, expr.op, left, right, out);
		}

		static bool evalRelation(BcState& state, Expr& expr, BcNum& out) {
			BcNum left;
			BcNum right;
			if (!eval(state, *expr.left, left) || !eval(state, *expr.right, right)) return false;

			out = bcFromInt(compareRelation(expr.relation, bcCompare(left, right)) ? 1 : 0);
			return true;
		}

		static bool evalTruth(BcState& state, Expr& expr, bool& truth) {
			BcNum value;
			if (!eval(state, expr, value)) return false;

			truth = !bcIsZero(value);
			return true;
		}

		static bool evalLogical(BcState& state, Expr& expr, bool is_and, BcNum& out) {
			bool truth = false;
			if (!evalTruth(state, *expr.left, truth)) return false;
			if (truth == is_and && !evalTruth(state, *expr.right, truth)) return false;

			out = bcFromInt(truth ? 1 : 0);
			return true;
		}

		static bool evalUnary(BcState& state, Expr& expr, BcNum& out) {
			BcNum value;
			if (!eval(state, *expr.left, value)) return false;

			switch (expr.kind) {
				case ExprKind::Negate:
					out = bcNegate(std::move(value));
					return true;
				case ExprKind::Not:
					out = bcFromInt(bcIsZero(value) ? 1 : 0);
					return true;
				case ExprKind::Length:
					out = bcFromInt(bcLength(value));
					return true;
				case ExprKind::ScaleOf:
					out = bcFromInt(value.scale);
					return true;
				default:
					return bcSqrt(value, state.scale, out) || runtimeError(state, "Square root of a negative number");
			}
		}

		static bool readInputLine(std::string& line) {
			line.clear();
			int c = 0;
			while ((c = std::fgetc(stdin)) != EOF && c != '\n') line.push_back(static_cast<char>(c));
			return c != EOF || !line.empty();
		}

		static bool evalRead(BcState& state, BcNum& out) {
			state.out.flush();

			std::string line;
			if (!readInputLine(line)) return runtimeError(state, "read() past end of input");

			ExprPtr parsed;
			if (!parseNumberText(state, line, parsed)) return runtimeError(state, "read() expected a number");
			return eval(state, *parsed, out);
		}

		static std::shared_ptr<BcArray> arrayArgument(BcState& state, const Param& param, int array_id) {
			const std::shared_ptr<BcArray>& source = currentArray(state, array_id);
			if (param.by_ref) return source;
			return std::make_shared<BcArray>(*source);
		}

		struct CallFrame {
			std::vector<BcNum> values;
			std::vector<std::shared_ptr<BcArray>> arrays;
		};

		static bool bindArguments(BcState& state, const Function& fn, std::vector<CallArg>& args, CallFrame& frame) {
			if (args.size() != fn.params.size()) return runtimeError(state, "Parameter number mismatch");

			frame.values.resize(args.size());
			frame.arrays.resize(args.size());
			for (std::size_t i = 0; i < args.size(); ++i) {
				const Param& param = fn.params[i];
				if (param.is_array != (args[i].array_id >= 0)) {
					return runtimeError(state, "Parameter type mismatch, parameter " + nameOf(state, param.id) + ".");
				}

				if (param.is_array) frame.arrays[i] = arrayArgument(state, param, args[i].array_id);
				else if (!eval(state, *args[i].value, frame.values[i])) return false;
			}

			return true;
		}

		static void pushParam(BcState& state, const Param& param, BcNum value, std::shared_ptr<BcArray> array) {
			const auto index = static_cast<std::size_t>(param.id);
			if (param.is_array) state.arrays[index].push_back(array ? std::move(array) : std::make_shared<BcArray>());
			else state.scalars[index].push_back(std::move(value));
		}

		static void popParam(BcState& state, const Param& param) {
			const auto index = static_cast<std::size_t>(param.id);
			if (param.is_array) state.arrays[index].pop_back();
			else state.scalars[index].pop_back();
		}

		static void pushFrame(BcState& state, const Function& fn, CallFrame& frame) {
			for (std::size_t i = 0; i < fn.params.size(); ++i) {
				pushParam(state, fn.params[i], std::move(frame.values[i]), std::move(frame.arrays[i]));
			}

			for (const Param& local : fn.autos) pushParam(state, local, BcNum(), nullptr);
			state.call_stack.push_back(fn.id);
			++state.call_depth;
		}

		static void popFrame(BcState& state, const Function& fn) {
			--state.call_depth;
			state.call_stack.pop_back();
			for (auto it = fn.autos.rbegin(); it != fn.autos.rend(); ++it) popParam(state, *it);
			for (auto it = fn.params.rbegin(); it != fn.params.rend(); ++it) popParam(state, *it);
		}

		static Flow execList(BcState& state, StmtList& list);

		static bool callUser(BcState& state, const std::shared_ptr<Function>& fn, std::vector<CallArg>& args,
				BcNum& out) {
			CallFrame frame;
			if (!bindArguments(state, *fn, args, frame)) return false;
			if (state.call_depth >= kMaxCallDepth) return runtimeError(state, "function call nesting too deep");

			pushFrame(state, *fn, frame);
			state.return_value = BcNum();
			const Flow flow = execList(state, fn->body);
			popFrame(state, *fn);
			if (flow == Flow::Abort) return false;

			out = flow == Flow::Return ? std::move(state.return_value) : BcNum();
			return true;
		}

		static std::shared_ptr<Function> userFunction(const BcState& state, int id) {
			const auto index = static_cast<std::size_t>(id);
			return index < state.functions.size() ? state.functions[index] : nullptr;
		}

		static bool callLibrary(BcState& state, Expr& expr, BcNum& out) {
			std::vector<BcNum> values(expr.args.size());
			for (std::size_t i = 0; i < expr.args.size(); ++i) {
				if (!expr.args[i].value) return runtimeError(state, "Parameter type mismatch");
				if (!eval(state, *expr.args[i].value, values[i])) return false;
			}

			return callMathLib(nameOf(state, expr.name_id), values, state.scale, out)
				|| runtimeError(state, "Parameter number mismatch");
		}

		static bool evalCall(BcState& state, Expr& expr, BcNum& out) {
			const std::shared_ptr<Function> fn = userFunction(state, expr.name_id);
			if (fn) return callUser(state, fn, expr.args, out);
			if (state.mathlib && isMathLibFunction(nameOf(state, expr.name_id))) return callLibrary(state, expr, out);
			return runtimeError(state, "Function " + nameOf(state, expr.name_id) + " not defined.");
		}

		static bool evalNamed(BcState& state, Expr& expr, BcNum& out) {
			LValue lvalue;
			if (!resolve(state, expr, lvalue)) return false;

			out = readLValue(state, lvalue);
			return true;
		}

		static bool eval(BcState& state, Expr& expr, BcNum& out) {
			switch (expr.kind) {
				case ExprKind::Number:
					return evalNumber(state, expr, out);
				case ExprKind::Assign:
					return evalAssign(state, expr, out);
				case ExprKind::PreIncrement:
					return evalStep(state, expr, 1, true, out);
				case ExprKind::PreDecrement:
					return evalStep(state, expr, -1, true, out);
				case ExprKind::PostIncrement:
					return evalStep(state, expr, 1, false, out);
				case ExprKind::PostDecrement:
					return evalStep(state, expr, -1, false, out);
				case ExprKind::Binary:
					return evalBinary(state, expr, out);
				case ExprKind::Relation:
					return evalRelation(state, expr, out);
				case ExprKind::And:
					return evalLogical(state, expr, true, out);
				case ExprKind::Or:
					return evalLogical(state, expr, false, out);
				case ExprKind::Call:
					return evalCall(state, expr, out);
				case ExprKind::Read:
					return evalRead(state, out);
				case ExprKind::Negate:
				case ExprKind::Not:
				case ExprKind::Length:
				case ExprKind::ScaleOf:
				case ExprKind::Sqrt:
					return evalUnary(state, expr, out);
				default:
					return evalNamed(state, expr, out);
			}
		}

		static bool isSilentExpression(const BcState& state, const Expr& expr) {
			if (expr.kind == ExprKind::Assign && !expr.parenthesized) return true;
			if (expr.kind != ExprKind::Call) return false;

			const std::shared_ptr<Function> fn = userFunction(state, expr.name_id);
			return fn && fn->is_void;
		}

		static Flow execExpression(BcState& state, Stmt& stmt) {
			BcNum value;
			if (!eval(state, *stmt.expr, value)) return Flow::Abort;
			if (isSilentExpression(state, *stmt.expr)) return Flow::Normal;

			writeNumber(state.out, value, state.obase);
			state.out.put('\n');
			state.last = std::move(value);
			return Flow::Normal;
		}

		static char printEscape(char c) {
			switch (c) {
				case 'a':
					return '\a';
				case 'b':
					return '\b';
				case 'f':
					return '\f';
				case 'n':
					return '\n';
				case 'q':
					return '"';
				case 'r':
					return '\r';
				case 't':
					return '\t';
				case 'e':
				case '\\':
					return '\\';
				default:
					return '\0';
			}
		}

		static std::string expandPrintEscapes(const std::string& text) {
			std::string result;
			for (std::size_t i = 0; i < text.size(); ++i) {
				if (text[i] != '\\' || i + 1 >= text.size()) {
					result.push_back(text[i]);
					continue;
				}

				const char escaped = printEscape(text[++i]);
				if (escaped != '\0') result.push_back(escaped);
			}

			return result;
		}

		static Flow execPrint(BcState& state, Stmt& stmt) {
			for (PrintItem& item : stmt.items) {
				if (!item.value) {
					state.out.write(expandPrintEscapes(item.text));
					continue;
				}

				BcNum value;
				if (!eval(state, *item.value, value)) return Flow::Abort;
				writeNumber(state.out, value, state.obase);
			}

			return Flow::Normal;
		}

		static Flow execIf(BcState& state, Stmt& stmt) {
			bool truth = false;
			if (!evalTruth(state, *stmt.expr, truth)) return Flow::Abort;
			if (truth) return exec(state, *stmt.body);
			return stmt.else_body ? exec(state, *stmt.else_body) : Flow::Normal;
		}

		static bool loopCondition(BcState& state, Stmt& stmt, bool& truth) {
			truth = true;
			return !stmt.expr || evalTruth(state, *stmt.expr, truth);
		}

		static bool evalDiscard(BcState& state, Expr* expr) {
			BcNum ignored;
			return expr == nullptr || eval(state, *expr, ignored);
		}

		static Flow execLoop(BcState& state, Stmt& stmt) {
			if (!evalDiscard(state, stmt.init.get())) return Flow::Abort;

			for (;;) {
				bool truth = false;
				if (!loopCondition(state, stmt, truth)) return Flow::Abort;
				if (!truth) return Flow::Normal;

				const Flow flow = exec(state, *stmt.body);
				if (flow == Flow::Break) return Flow::Normal;
				if (flow == Flow::Return || flow == Flow::Abort) return flow;
				if (!evalDiscard(state, stmt.step.get())) return Flow::Abort;
			}
		}

		static Flow execReturn(BcState& state, Stmt& stmt) {
			BcNum value;
			if (stmt.expr && !eval(state, *stmt.expr, value)) return Flow::Abort;

			state.return_value = std::move(value);
			return Flow::Return;
		}

		static void printLimits(BcOutput& out) {
			out.write("BC_BASE_MAX     = " + std::to_string(kMaxObase) + "\n");
			out.write("BC_DIM_MAX      = " + std::to_string(kMaxDim) + "\n");
			out.write("BC_SCALE_MAX    = " + std::to_string(kMaxScale) + "\n");
			out.write("BC_STRING_MAX   = 2147483647\n");
			out.write("MAX Exponent    = 9223372036854775807\n");
			out.write("Number of vars  = 2147483647\n");
		}

		static void printWarranty(BcOutput& out) {
			out.write("bc is part of wbsh and comes with ABSOLUTELY NO WARRANTY;\n");
			out.write("see the LICENSE file distributed with wbsh for details.\n");
		}

		static Flow execOutput(BcState& state, Stmt& stmt) {
			switch (stmt.kind) {
				case StmtKind::String:
					state.out.write(stmt.text);
					return Flow::Normal;
				case StmtKind::Limits:
					printLimits(state.out);
					return Flow::Normal;
				case StmtKind::Warranty:
					printWarranty(state.out);
					return Flow::Normal;
				default:
					return execPrint(state, stmt);
			}
		}

		static Flow exec(BcState& state, Stmt& stmt) {
			switch (stmt.kind) {
				case StmtKind::Expression:
					return execExpression(state, stmt);
				case StmtKind::Block:
					return execList(state, stmt.list);
				case StmtKind::If:
					return execIf(state, stmt);
				case StmtKind::While:
				case StmtKind::For:
					return execLoop(state, stmt);
				case StmtKind::Break:
					return Flow::Break;
				case StmtKind::Continue:
					return Flow::Continue;
				case StmtKind::Return:
					return execReturn(state, stmt);
				case StmtKind::Halt:
					state.halted = true;
					return Flow::Abort;
				case StmtKind::String:
				case StmtKind::Print:
				case StmtKind::Limits:
				case StmtKind::Warranty:
					return execOutput(state, stmt);
				default:
					return Flow::Normal;
			}
		}

		static Flow execList(BcState& state, StmtList& list) {
			for (StmtPtr& stmt : list) {
				const Flow flow = exec(state, *stmt);
				if (flow != Flow::Normal) return flow;
			}

			return Flow::Normal;
		}

		bool runItem(BcState& state, StmtList& item) {
			execList(state, item);
			return !state.halted;
		}

	}  // namespace bc_detail

}  // namespace wbsh
