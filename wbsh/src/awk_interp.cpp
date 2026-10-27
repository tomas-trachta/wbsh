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

#include "regexutil.h"

namespace wbsh {

	namespace awk_detail {

		// Keeps deep awk recursion inside the interpreter thread's stack (see awk.cpp).
		static const int kMaxCallDepth = 30000;

		template <typename T>
		static bool compareWith(const T& left, const T& right, const std::string& op) {
			if (op == "==") return left == right;
			if (op == "!=") return left != right;
			if (op == "<")  return left < right;
			if (op == "<=") return left <= right;
			if (op == ">")  return left > right;
			return left >= right;
		}

		static bool isRelationalOp(const std::string& op) {
			return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
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
				case kNF:  return AwkValue::num(static_cast<double>(fields.size()));
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
				case kFS:      FS = text(value); break;
				case kOFS:     OFS = text(value); break;
				case kORS:     ORS = text(value); break;
				case kRS:      RS = text(value); break;
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
			if (var.local) return frames.back()->locals[static_cast<std::size_t>(var.slot)];
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

		const std::regex* Interpreter::literalRegex(const Expr& expr) {
			if (expr.compiled) return expr.compiled.get();
			if (expr.compile_failed) return nullptr;

			auto re = std::make_shared<std::regex>();
			if (!compileRegex(*re, expr.text)) {
				expr.compile_failed = true;
				return nullptr;
			}

			expr.compiled = std::move(re);
			return expr.compiled.get();
		}

		const std::regex* Interpreter::dynamicRegex(const std::string& pattern,
				std::regex& storage) {
			if (!compileRegex(storage, pattern)) return nullptr;
			return &storage;
		}

		AwkValue Interpreter::evalUnary(Expr& expr) {
			if (expr.op == "++" || expr.op == "--") {
				const double current = eval(*expr.first).asNumber();
				const double updated = (expr.op == "++") ? current + 1 : current - 1;
				assignLValue(*expr.first, AwkValue::num(updated));
				return AwkValue::num(updated);
			}

			const AwkValue operand = eval(*expr.first);
			if (expr.op == "!") return boolValue(!operand.truthy());
			if (expr.op == "-") return AwkValue::num(-operand.asNumber());
			return AwkValue::num(operand.asNumber());
		}

		AwkValue Interpreter::evalPostIncDec(Expr& expr) {
			const double current = eval(*expr.first).asNumber();
			const double updated = (expr.op == "++") ? current + 1 : current - 1;
			assignLValue(*expr.first, AwkValue::num(updated));
			return AwkValue::num(current);
		}

		AwkValue Interpreter::evalRegexMatch(const std::string& subject, Expr& pattern,
				bool positive) {
			std::regex storage;
			const std::regex* re = pattern.kind == ExprKind::Regex
				? literalRegex(pattern)
				: dynamicRegex(text(eval(pattern)), storage);
			const bool matched = re != nullptr && searchRegex(subject, *re);
			return boolValue(positive ? matched : !matched);
		}

		AwkValue Interpreter::evalRegexLiteral(Expr& expr) {
			const std::regex* re = literalRegex(expr);
			return boolValue(re != nullptr && searchRegex(record, *re));
		}

		static AwkValue compareValues(const AwkValue& left, const AwkValue& right,
				const std::string& op, const std::string& convfmt) {
			const bool numeric = left.comparesAsNumber() && right.comparesAsNumber();
			const int order = numeric
				? orderNumbers(left.asNumber(), right.asNumber())
				: orderStrings(left.asString(convfmt), right.asString(convfmt));
			return boolValue(compareWith(order, 0, op));
		}

		AwkValue Interpreter::evalArithmetic(const std::string& op, const AwkValue& left,
				const AwkValue& right) {
			if (op == "+") return AwkValue::num(left.asNumber() + right.asNumber());
			if (op == "-") return AwkValue::num(left.asNumber() - right.asNumber());
			if (op == "*") return AwkValue::num(left.asNumber() * right.asNumber());
			if (op == "/") {
				const double divisor = right.asNumber();
				return AwkValue::num(divisor == 0 ? 0.0 : left.asNumber() / divisor);
			}

			if (op == "%") {
				const double divisor = right.asNumber();
				return AwkValue::num(divisor == 0 ? 0.0 : std::fmod(left.asNumber(), divisor));
			}

			if (op == "^") return AwkValue::num(std::pow(left.asNumber(), right.asNumber()));
			if (isRelationalOp(op)) return compareValues(left, right, op, CONVFMT);
			return AwkValue::str("");
		}

		AwkValue Interpreter::evalBinary(Expr& expr) {
			if (expr.op == "&&") {
				if (!eval(*expr.first).truthy()) return AwkValue::num(0.0);
				return boolValue(eval(*expr.second).truthy());
			}

			if (expr.op == "||") {
				if (eval(*expr.first).truthy()) return AwkValue::num(1.0);
				return boolValue(eval(*expr.second).truthy());
			}

			if (expr.op == "~" || expr.op == "!~") {
				const std::string subject = text(eval(*expr.first));
				return evalRegexMatch(subject, *expr.second, expr.op == "~");
			}

			if (expr.op == " ") {
				std::string joined = text(eval(*expr.first));
				joined += text(eval(*expr.second));
				return AwkValue::str(std::move(joined));
			}

			const AwkValue left = eval(*expr.first);
			const AwkValue right = eval(*expr.second);
			return evalArithmetic(expr.op, left, right);
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
				case ExprKind::PostIncDec:  return evalPostIncDec(expr);
				case ExprKind::Binary:      return evalBinary(expr);
				case ExprKind::Ternary:     return evalTernary(expr);
				case ExprKind::Assign:      return evalAssign(expr);
				case ExprKind::Call:        return callBuiltin(expr);
				case ExprKind::UserCall:    return callUser(expr);
				case ExprKind::Getline:     return doGetline(expr);
				default:                    return AwkValue();
			}
		}

		AwkValue Interpreter::applyOp(const AwkValue& current, const std::string& op,
				const AwkValue& rhs) {
			if (op == "=") return rhs;

			const double left = current.asNumber();
			const double right = rhs.asNumber();
			if (op == "+=") return AwkValue::num(left + right);
			if (op == "-=") return AwkValue::num(left - right);
			if (op == "*=") return AwkValue::num(left * right);
			if (op == "/=") return AwkValue::num(right == 0 ? 0 : left / right);
			if (op == "%=") return AwkValue::num(right == 0 ? 0 : std::fmod(left, right));
			if (op == "^=") return AwkValue::num(std::pow(left, right));
			return rhs;
		}

		AwkValue Interpreter::assignTo(Expr& lhs, const std::string& op, AwkValue rhs) {
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
			assignTo(lhs, "=", std::move(value));
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

		AwkValue Interpreter::callUser(Expr& call) {
			if (call_depth >= kMaxCallDepth) {
				runtimeError("function '" + call.name + "': call nesting too deep");
				return AwkValue();
			}

			const FunctionDef& function = *call.function;
			auto frame = std::make_unique<Frame>();
			frame->locals.resize(std::max(function.params.size(), call.args.size()));
			for (std::size_t i = 0; i < call.args.size(); ++i) {
				bindArgument(*call.args[i], frame->locals[i]);
			}

			frames.push_back(std::move(frame));
			++call_depth;
			run(*function.body);
			--call_depth;
			frames.pop_back();

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
