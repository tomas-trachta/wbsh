/**
 * @file awk_interp.cpp
 * @brief awk interpreter: variables, expression evaluation, statements, rules, user functions.
 */

#include "awk_interp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	namespace awk_detail {

		// Keeps deep awk recursion inside the interpreter thread's stack (see awk.cpp).
		static const int kMaxCallDepth = 30000;

		static bool orderSatisfies(int order, Op op) {
			switch (op) {
				case Op::Eq: return order == 0;
				case Op::Ne: return order != 0;
				case Op::Lt: return order < 0;
				case Op::Le: return order <= 0;
				case Op::Gt: return order > 0;
				default:     return order >= 0;
			}
		}

		static bool isRelationalOp(Op op) {
			return op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge
				|| op == Op::Eq || op == Op::Ne;
		}

		// As in gawk, NaN equals NaN and sorts above every number.
		static int orderNumbers(double left, double right) {
			if (std::isnan(left)) return std::isnan(right) ? 0 : 1;
			if (std::isnan(right)) return -1;
			if (left < right) return -1;
			return left > right ? 1 : 0;
		}

		static int orderStrings(const std::string& left, const std::string& right) {
			const int order = left.compare(right);
			if (order < 0) return -1;
			return order > 0 ? 1 : 0;
		}

		static bool isSpecialBlock(const AwkPattern& pattern) {
			return pattern.kind == AwkPattern::Begin || pattern.kind == AwkPattern::End;
		}

		Interpreter::Interpreter(AwkProgram& parsed, Executor& executor)
			: program(parsed), exec(executor), globals(parsed.global_names.size()) {
			initSpecialVars();
		}

		Interpreter::~Interpreter() {
			closeAllStreams();
		}

		void Interpreter::initSpecialVars() {
			globals[kFS].value = AwkValue::str(FS);
			globals[kOFS].value = AwkValue::str(OFS);
			globals[kORS].value = AwkValue::str(ORS);
			globals[kRS].value = AwkValue::str(RS);
			globals[kSUBSEP].value = AwkValue::str(SUBSEP);
			globals[kCONVFMT].value = AwkValue::str(CONVFMT);
			globals[kOFMT].value = AwkValue::str(OFMT);
			globals[kRSTART].value = AwkValue::num(0);
			globals[kRLENGTH].value = AwkValue::num(-1);
		}

		void Interpreter::runtimeError(const std::string& message) {
			std::fflush(stdout);
			std::fprintf(stderr, "awk: %s\n", message.c_str());
			exiting = true;
			fatal = true;
			exit_status = kUsageError;
		}

		int Interpreter::globalSlot(const std::string& name) const {
			for (std::size_t i = 0; i < program.global_names.size(); ++i) {
				if (program.global_names[i] == name) return static_cast<int>(i);
			}

			return -1;
		}

		std::string Interpreter::text(const AwkValue& value) const {
			return value.asString(CONVFMT);
		}

		std::string Interpreter::outputText(const AwkValue& value) const {
			if (value.is_number && !value.is_string) return numberToString(value.number, OFMT);
			return text(value);
		}

		AwkValue Interpreter::getGlobal(int slot) {
			switch (slot) {
				case kNF:
					ensureFields();
					return AwkValue::num(static_cast<double>(fields.size()));
				case kNR:  return AwkValue::num(static_cast<double>(NR));
				case kFNR: return AwkValue::num(static_cast<double>(FNR));
				default:   return globals[static_cast<std::size_t>(slot)].value;
			}
		}

		void Interpreter::onSpecialAssigned(int slot) {
			const AwkValue& value = globals[static_cast<std::size_t>(slot)].value;
			switch (slot) {
				case kNF:      setFieldCount(static_cast<int>(value.asNumber())); break;
				case kNR:      NR = static_cast<long long>(value.asNumber()); break;
				case kFNR:     FNR = static_cast<long long>(value.asNumber()); break;
				case kFS:
					ensureFields();
					FS = text(value);
					break;
				case kOFS:     OFS = text(value); break;
				case kORS:     ORS = text(value); break;
				case kRS:
					ensureFields();
					RS = text(value);
					break;
				case kSUBSEP:  SUBSEP = text(value); break;
				case kCONVFMT: CONVFMT = text(value); break;
				case kOFMT:    OFMT = text(value); break;
				default:       break;
			}
		}

		void Interpreter::setGlobal(int slot, AwkValue value) {
			globals[static_cast<std::size_t>(slot)].value = std::move(value);
			if (slot < kSpecialCount) onSpecialAssigned(slot);
		}

		static AwkArray& arrayOf(Cell& cell) {
			if (!cell.array) cell.array = std::make_shared<AwkArray>();
			return *cell.array;
		}

		AwkArray& Interpreter::globalArray(int slot) {
			return arrayOf(globals[static_cast<std::size_t>(slot)]);
		}

		Cell& Interpreter::cellFor(const Expr& var) {
			if (var.local) {
				Frame& frame = *frames[static_cast<std::size_t>(call_depth) - 1];
				return frame.locals[static_cast<std::size_t>(var.slot)];
			}

			return globals[static_cast<std::size_t>(var.slot)];
		}

		AwkArray& Interpreter::arrayFor(const Expr& var) {
			return arrayOf(cellFor(var));
		}

		AwkValue Interpreter::getVariable(const Expr& var) {
			if (var.local) return cellFor(var).value;
			return getGlobal(var.slot);
		}

		void Interpreter::setVariable(const Expr& var, AwkValue value) {
			if (var.local) {
				cellFor(var).value = std::move(value);
				return;
			}

			setGlobal(var.slot, std::move(value));
		}

		std::string Interpreter::buildSubscript(const std::vector<Expr*>& subscripts) {
			std::string key;
			for (std::size_t i = 0; i < subscripts.size(); ++i) {
				if (i != 0) key += SUBSEP;
				key += text(eval(*subscripts[i]));
			}

			return key;
		}

		const AwkRegex* Interpreter::literalRegex(const Expr& expr) {
			if (!expr.compiled) {
				auto compiled = std::make_shared<AwkRegex>();
				compileAwkRegex(*compiled, expr.text);
				expr.compiled = std::move(compiled);
			}

			return expr.compiled->ok ? expr.compiled.get() : nullptr;
		}

		std::shared_ptr<const AwkRegex> Interpreter::dynamicRegex(const std::string& pattern) {
			return regex_cache.get(pattern);
		}

		// A regex literal operand is the regex itself; any other value is
		// compiled from its string.
		std::shared_ptr<const AwkRegex> Interpreter::regexOperand(Expr& expr) {
			if (expr.kind != ExprKind::Regex) return dynamicRegex(text(eval(expr)));

			literalRegex(expr);
			return expr.compiled;
		}

		AwkValue Interpreter::evalUnary(Expr& expr) {
			switch (expr.op) {
				case Op::PreIncrement: return evalIncDec(*expr.first, 1, false);
				case Op::PreDecrement: return evalIncDec(*expr.first, -1, false);
				case Op::Not:          return boolValue(!eval(*expr.first).truthy());
				case Op::Negate:       return AwkValue::num(-eval(*expr.first).asNumber());
				default:               return AwkValue::num(eval(*expr.first).asNumber());
			}
		}

		// The target's subscript or field index is evaluated once.
		AwkValue Interpreter::evalIncDec(Expr& target, double delta, bool post) {
			if (target.kind == ExprKind::ArrayRef) {
				const std::string key = buildSubscript(target.args);
				AwkValue& slot = arrayFor(target)[key];
				const double current = slot.asNumber();
				slot = AwkValue::num(current + delta);
				return AwkValue::num(post ? current : current + delta);
			}

			if (target.kind == ExprKind::Field) {
				const int index = fieldIndex(*target.first);
				const double current = getField(index).asNumber();
				setField(index, AwkValue::num(current + delta));
				return AwkValue::num(post ? current : current + delta);
			}

			const double current = getVariable(target).asNumber();
			setVariable(target, AwkValue::num(current + delta));
			return AwkValue::num(post ? current : current + delta);
		}

		AwkValue Interpreter::evalRegexMatch(const std::string& subject, Expr& pattern,
				bool positive) {
			const std::shared_ptr<const AwkRegex> re = regexOperand(pattern);
			const bool matched = awkRegexSearch(*re, subject);
			return boolValue(positive ? matched : !matched);
		}

		AwkValue Interpreter::evalRegexLiteral(Expr& expr) {
			const AwkRegex* re = literalRegex(expr);
			return boolValue(re != nullptr && awkRegexSearch(*re, record));
		}

		static AwkValue compareValues(const AwkValue& left, const AwkValue& right, Op op,
				const std::string& convfmt) {
			const bool numeric = left.comparesAsNumber() && right.comparesAsNumber();
			const int order = numeric
				? orderNumbers(left.asNumber(), right.asNumber())
				: orderStrings(left.asString(convfmt), right.asString(convfmt));
			return boolValue(orderSatisfies(order, op));
		}

		static double divide(double left, double right) {
			return right == 0 ? 0.0 : left / right;
		}

		static double modulo(double left, double right) {
			return right == 0 ? 0.0 : std::fmod(left, right);
		}

		AwkValue Interpreter::evalArithmetic(Op op, const AwkValue& left, const AwkValue& right) {
			if (isRelationalOp(op)) return compareValues(left, right, op, CONVFMT);

			const double x = left.asNumber();
			const double y = right.asNumber();
			switch (op) {
				case Op::Add: return AwkValue::num(x + y);
				case Op::Sub: return AwkValue::num(x - y);
				case Op::Mul: return AwkValue::num(x * y);
				case Op::Div: return AwkValue::num(divide(x, y));
				case Op::Mod: return AwkValue::num(modulo(x, y));
				case Op::Pow: return AwkValue::num(std::pow(x, y));
				default:      return AwkValue::str("");
			}
		}

		AwkValue Interpreter::evalBinary(Expr& expr) {
			switch (expr.op) {
				case Op::And:
					if (!eval(*expr.first).truthy()) return AwkValue::num(0.0);
					return boolValue(eval(*expr.second).truthy());
				case Op::Or:
					if (eval(*expr.first).truthy()) return AwkValue::num(1.0);
					return boolValue(eval(*expr.second).truthy());
				case Op::Match:
				case Op::NoMatch:
					return evalRegexMatch(text(eval(*expr.first)), *expr.second,
						expr.op == Op::Match);
				case Op::Concat: {
					std::string joined = text(eval(*expr.first));
					joined += text(eval(*expr.second));
					return AwkValue::str(std::move(joined));
				}
				default: {
					const AwkValue left = eval(*expr.first);
					const AwkValue right = eval(*expr.second);
					return evalArithmetic(expr.op, left, right);
				}
			}
		}

		int Interpreter::fieldIndex(Expr& index_expr) {
			return static_cast<int>(eval(index_expr).asNumber());
		}

		AwkValue Interpreter::evalTernary(Expr& expr) {
			if (eval(*expr.first).truthy()) return eval(*expr.second);
			return eval(*expr.third);
		}

		AwkValue Interpreter::evalArrayRef(Expr& expr) {
			const std::string key = buildSubscript(expr.args);
			return arrayFor(expr)[key];
		}

		AwkValue Interpreter::evalArrayInTest(Expr& expr) {
			const std::string key = expr.args.empty()
				? text(eval(*expr.first))
				: buildSubscript(expr.args);
			const AwkArray& array = arrayFor(expr);
			return boolValue(array.count(key) != 0);
		}

		AwkValue Interpreter::evalAssign(Expr& expr) {
			return assignTo(*expr.first, expr.op, eval(*expr.second));
		}

		AwkValue Interpreter::eval(Expr& expr) {
			switch (expr.kind) {
				case ExprKind::Number:      return AwkValue::num(expr.number);
				case ExprKind::String:      return AwkValue::str(expr.text);
				case ExprKind::Regex:       return evalRegexLiteral(expr);
				case ExprKind::Var:         return getVariable(expr);
				case ExprKind::Field:       return getField(fieldIndex(*expr.first));
				case ExprKind::ArrayRef:    return evalArrayRef(expr);
				case ExprKind::ArrayInTest: return evalArrayInTest(expr);
				case ExprKind::Group:       return eval(*expr.first);
				case ExprKind::Unary:       return evalUnary(expr);
				case ExprKind::PostIncDec:
					return evalIncDec(*expr.first, expr.op == Op::PostIncrement ? 1 : -1, true);
				case ExprKind::Binary:      return evalBinary(expr);
				case ExprKind::Ternary:     return evalTernary(expr);
				case ExprKind::Assign:      return evalAssign(expr);
				case ExprKind::Call:        return callBuiltin(expr);
				case ExprKind::UserCall:    return callUser(expr);
				case ExprKind::Getline:     return doGetline(expr);
				default:                    return AwkValue();
			}
		}

		AwkValue Interpreter::applyOp(const AwkValue& current, Op op, const AwkValue& rhs) {
			if (op == Op::Assign) return rhs;

			const double left = current.asNumber();
			const double right = rhs.asNumber();
			switch (op) {
				case Op::AddAssign: return AwkValue::num(left + right);
				case Op::SubAssign: return AwkValue::num(left - right);
				case Op::MulAssign: return AwkValue::num(left * right);
				case Op::DivAssign: return AwkValue::num(divide(left, right));
				case Op::ModAssign: return AwkValue::num(modulo(left, right));
				case Op::PowAssign: return AwkValue::num(std::pow(left, right));
				default:            return rhs;
			}
		}

		AwkValue Interpreter::assignTo(Expr& lhs, Op op, AwkValue rhs) {
			if (lhs.kind == ExprKind::Var) {
				AwkValue result = applyOp(getVariable(lhs), op, rhs);
				setVariable(lhs, result);
				return result;
			}

			if (lhs.kind == ExprKind::ArrayRef) {
				const std::string key = buildSubscript(lhs.args);
				AwkValue& slot = arrayFor(lhs)[key];
				slot = applyOp(slot, op, rhs);
				return slot;
			}

			if (lhs.kind == ExprKind::Field) {
				const int index = fieldIndex(*lhs.first);
				AwkValue result = applyOp(getField(index), op, rhs);
				setField(index, result);
				return result;
			}

			return rhs;
		}

		void Interpreter::assignLValue(Expr& lhs, AwkValue value) {
			assignTo(lhs, Op::Assign, std::move(value));
		}

		// A bare variable without a scalar value (an array, or nothing yet) is
		// passed by reference so the callee can use it as an array.
		bool Interpreter::passesByReference(const Expr& arg) {
			if (arg.kind != ExprKind::Var) return false;
			if (!arg.local && arg.slot < kSpecialCount) {
				return arg.slot == kENVIRON || arg.slot == kARGV;
			}

			return cellFor(arg).value.isUninitialised();
		}

		void Interpreter::bindArgument(Expr& arg, Cell& param) {
			if (!passesByReference(arg)) {
				param.value = eval(arg);
				return;
			}

			Cell& source = cellFor(arg);
			arrayOf(source);
			param.array = source.array;
		}

		// Arguments are bound before the frame becomes current: they are
		// evaluated in the caller's scope.
		Frame& Interpreter::acquireFrame() {
			const auto depth = static_cast<std::size_t>(call_depth);
			if (depth == frames.size()) frames.push_back(std::make_unique<Frame>());

			return *frames[depth];
		}

		AwkValue Interpreter::callUser(Expr& call) {
			if (call_depth >= kMaxCallDepth) {
				runtimeError("function '" + call.name + "': call nesting too deep");
				return AwkValue();
			}

			const FunctionDef& function = *call.function;
			std::vector<Cell> locals(std::max(function.params.size(), call.args.size()));
			for (std::size_t i = 0; i < call.args.size(); ++i) {
				bindArgument(*call.args[i], locals[i]);
			}

			Frame& frame = acquireFrame();
			frame.locals.swap(locals);
			++call_depth;
			run(*function.body);
			--call_depth;
			frame.locals.clear();

			if (flow == Flow::Return) flow = Flow::None;
			AwkValue result = std::move(return_value);
			return_value = AwkValue();
			return result;
		}

		bool Interpreter::loopBodyDone() {
			if (flow == Flow::Break) {
				flow = Flow::None;
				return true;
			}

			if (flow == Flow::Continue) flow = Flow::None;
			return flow != Flow::None || exiting;
		}

		void Interpreter::runIf(Stmt& stmt) {
			if (eval(*stmt.exprs[0]).truthy()) {
				if (!stmt.children.empty()) run(*stmt.children[0]);
				return;
			}

			if (stmt.children.size() >= 2) run(*stmt.children[1]);
		}

		void Interpreter::runWhile(Stmt& stmt) {
			while (!exiting && eval(*stmt.exprs[0]).truthy()) {
				run(*stmt.children[0]);
				if (loopBodyDone()) return;
			}
		}

		void Interpreter::runDoWhile(Stmt& stmt) {
			do {
				run(*stmt.children[0]);
				if (loopBodyDone()) return;
			} while (!exiting && eval(*stmt.exprs[0]).truthy());
		}

		void Interpreter::runFor(Stmt& stmt) {
			if (stmt.init != nullptr) eval(*stmt.init);
			while (!exiting && (stmt.cond == nullptr || eval(*stmt.cond).truthy())) {
				run(*stmt.children[0]);
				if (loopBodyDone()) return;
				if (stmt.step != nullptr) eval(*stmt.step);
			}
		}

		// The body may add or delete elements, so the keys are snapshotted first.
		void Interpreter::runForIn(Stmt& stmt) {
			std::vector<std::string> keys;
			for (const auto& entry : arrayFor(*stmt.exprs[1])) keys.push_back(entry.first);

			for (const auto& key : keys) {
				setVariable(*stmt.exprs[0], AwkValue::str(key));
				run(*stmt.children[0]);
				if (loopBodyDone()) return;
			}
		}

		void Interpreter::runDelete(Stmt& stmt) {
			Expr& target = *stmt.exprs[0];
			if (target.kind == ExprKind::Var) {
				arrayFor(target).clear();
				return;
			}

			const std::string key = buildSubscript(target.args);
			arrayFor(target).erase(key);
		}

		void Interpreter::runPrint(Stmt& stmt) {
			std::string out;
			if (stmt.exprs.empty()) {
				out = record;
			} else {
				for (std::size_t i = 0; i < stmt.exprs.size(); ++i) {
					if (i != 0) out += OFS;
					out += outputText(eval(*stmt.exprs[i]));
				}
			}

			out += ORS;
			emit(out, stmt);
		}

		std::vector<AwkValue> Interpreter::evalAll(const std::vector<Expr*>& exprs) {
			std::vector<AwkValue> values;
			values.reserve(exprs.size());
			for (auto& expr : exprs) values.push_back(eval(*expr));
			return values;
		}

		void Interpreter::runPrintf(Stmt& stmt) {
			emit(formatPrintf(evalAll(stmt.exprs), CONVFMT), stmt);
		}

		void Interpreter::runBlock(Stmt& stmt) {
			for (auto& child : stmt.children) {
				run(*child);
				if (exiting || flow != Flow::None) return;
			}
		}

		void Interpreter::runExit(Stmt& stmt) {
			if (!stmt.exprs.empty()) {
				exit_status = static_cast<int>(eval(*stmt.exprs[0]).asNumber());
			}

			exiting = true;
		}

		void Interpreter::runReturn(Stmt& stmt) {
			return_value = stmt.exprs.empty() ? AwkValue() : eval(*stmt.exprs[0]);
			flow = Flow::Return;
		}

		void Interpreter::run(Stmt& stmt) {
			if (exiting) return;
			switch (stmt.kind) {
				case StmtKind::Empty:    return;
				case StmtKind::Block:    runBlock(stmt);   return;
				case StmtKind::If:       runIf(stmt);      return;
				case StmtKind::While:    runWhile(stmt);   return;
				case StmtKind::DoWhile:  runDoWhile(stmt); return;
				case StmtKind::For:      runFor(stmt);     return;
				case StmtKind::ForIn:    runForIn(stmt);   return;
				case StmtKind::Break:    flow = Flow::Break;    return;
				case StmtKind::Continue: flow = Flow::Continue; return;
				case StmtKind::Next:     flow = Flow::Next;     return;
				case StmtKind::Exit:     runExit(stmt);    return;
				case StmtKind::Delete:   runDelete(stmt);  return;
				case StmtKind::Return:   runReturn(stmt);  return;
				case StmtKind::Print:    runPrint(stmt);   return;
				case StmtKind::Printf:   runPrintf(stmt);  return;
				case StmtKind::ExprStmt: eval(*stmt.exprs[0]); return;
				default:                 return;
			}
		}

		bool Interpreter::patternHolds(Expr& expr) {
			return eval(expr).truthy();
		}

		bool Interpreter::rangeHolds(AwkRule& rule) {
			if (!rule.in_range) {
				if (!patternHolds(*rule.pattern.first)) return false;

				rule.in_range = !patternHolds(*rule.pattern.second);
				return true;
			}

			if (patternHolds(*rule.pattern.second)) rule.in_range = false;
			return true;
		}

		bool Interpreter::ruleMatches(AwkRule& rule) {
			switch (rule.pattern.kind) {
				case AwkPattern::Always:    return true;
				case AwkPattern::Condition: return patternHolds(*rule.pattern.first);
				case AwkPattern::Range:     return rangeHolds(rule);
				default:                    return false;
			}
		}

		void Interpreter::runRecordRules() {
			for (auto& rule : program.rules) {
				if (isSpecialBlock(rule.pattern)) continue;

				if (ruleMatches(rule)) run(*rule.action);
				if (exiting || flow == Flow::Next) break;
			}

			flow = Flow::None;
		}

		void Interpreter::runSpecialBlocks(AwkPattern::Kind kind) {
			for (auto& rule : program.rules) {
				if (rule.pattern.kind != kind) continue;

				run(*rule.action);
				flow = Flow::None;
				if (exiting) return;
			}
		}

		void Interpreter::runBegins() {
			runSpecialBlocks(AwkPattern::Begin);
		}

		void Interpreter::runMainInput() {
			std::string line;
			while (!exiting && readMainRecord(line)) {
				++NR;
				++FNR;
				setRecord(std::move(line));
				runRecordRules();
			}
		}

		// `exit` in BEGIN or a main rule still runs the END rules; one in END stops them.
		void Interpreter::runEnds() {
			if (fatal) return;

			exiting = false;
			runSpecialBlocks(AwkPattern::End);
		}

	}  // namespace awk_detail

}  // namespace wbsh
