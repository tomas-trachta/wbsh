#pragma once

/**
 * @file awk_interp.h
 * @brief The tree-walking awk interpreter; its members are spread over
 *        awk_interp.cpp (evaluation), awk_fields.cpp (records and fields),
 *        awk_builtins.cpp (builtin functions) and awk_io.cpp (input and output).
 */

#include <cstdio>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "awk_internal.h"

namespace wbsh {

	class Executor;

	namespace awk_detail {

		// A variable can be a scalar and, once it is used as one, an array; the
		// array is shared so a function parameter can alias its argument's.
		struct Cell {
			AwkValue value;
			std::shared_ptr<AwkArray> array;
		};

		struct Frame {
			std::vector<Cell> locals;
		};

		struct InputStream {
			std::FILE* file = nullptr;
			bool owned = false;
			std::string buffer;
			std::size_t start = 0;

			InputStream() = default;
			InputStream(const InputStream&) = delete;
			InputStream& operator=(const InputStream&) = delete;

			~InputStream() {
				if (owned && file != nullptr) std::fclose(file);
			}
		};

		struct OutputStream {
			std::FILE* file = nullptr;
			bool owned = false;
			bool is_pipe = false;
			std::string command;
			// Text for a `print | cmd` pipe, fed to the command when it is closed.
			std::string pending;
		};

		struct CachedRegex {
			std::string pattern;
			std::regex regex;
			bool ok = false;
			bool valid = false;
		};

		std::vector<std::string> splitOnBlanks(const std::string& text);
		std::vector<std::string> splitOnChar(const std::string& text, char separator);
		std::vector<std::string> splitByRegex(const std::string& text, const std::regex& re);

		struct Interpreter {
			enum class Flow { None, Break, Continue, Next, Return };

			AwkProgram& program;
			Executor& exec;
			std::vector<Cell> globals;
			std::vector<std::unique_ptr<Frame>> frames;
			std::vector<AwkValue> fields;
			std::string record;
			long long NR = 0;
			long long FNR = 0;
			std::string FS = " ";
			std::string OFS = " ";
			std::string ORS = "\n";
			std::string RS = "\n";
			std::string SUBSEP = "\x1c";
			std::string CONVFMT = kDefaultNumberFormat;
			std::string OFMT = kDefaultNumberFormat;
			bool exiting = false;
			bool fatal = false;
			bool input_error = false;
			int exit_status = 0;
			Flow flow = Flow::None;
			AwkValue return_value;
			int call_depth = 0;
			unsigned random_seed = 0;
			CachedRegex fs_regex;
			CachedRegex rs_regex;
			std::map<std::string, std::unique_ptr<OutputStream>> outputs;
			std::vector<std::string> output_order;
			std::map<std::string, std::unique_ptr<InputStream>> input_files;
			std::map<std::string, std::unique_ptr<InputStream>> input_commands;
			std::unique_ptr<InputStream> main_input;
			int next_operand = 1;
			bool opened_operand = false;

			Interpreter(AwkProgram& parsed, Executor& executor);
			~Interpreter();

			// awk_interp.cpp
			void runBegins();
			void runMainInput();
			void runEnds();
			void runtimeError(const std::string& message);
			int globalSlot(const std::string& name) const;
			std::string text(const AwkValue& value) const;
			std::string outputText(const AwkValue& value) const;
			AwkValue getGlobal(int slot);
			void setGlobal(int slot, AwkValue value);
			AwkArray& globalArray(int slot);
			Cell& cellFor(const Expr& var);
			AwkArray& arrayFor(const Expr& var);
			AwkValue getVariable(const Expr& var);
			void setVariable(const Expr& var, AwkValue value);
			AwkValue eval(Expr& expr);
			void assignLValue(Expr& lhs, AwkValue value);
			std::string buildSubscript(const std::vector<Expr*>& subscripts);
			const std::regex* literalRegex(const Expr& expr);
			const std::regex* dynamicRegex(const std::string& pattern, std::regex& storage);
			void run(Stmt& stmt);

			// awk_fields.cpp
			void setRecord(std::string text);
			void splitRecord();
			AwkValue getField(int index);
			void setField(int index, AwkValue value);
			void setFieldCount(int count);
			std::vector<std::string> splitWithSeparator(const std::string& text,
				const std::string& separator);

			// awk_builtins.cpp
			AwkValue callBuiltin(Expr& call);

			// awk_io.cpp
			AwkValue doGetline(Expr& expr);
			void emit(const std::string& out, Stmt& stmt);
			int closeStream(const std::string& name);
			void closeAllStreams();
			void flushAllOutput();
			int runShellCommand(const std::string& command);

		private:
			void onSpecialAssigned(int slot);
			void initSpecialVars();
			AwkValue evalUnary(Expr& expr);
			AwkValue evalPostIncDec(Expr& expr);
			AwkValue evalRegexMatch(const std::string& subject, Expr& pattern, bool positive);
			AwkValue evalArithmetic(const std::string& op, const AwkValue& left,
				const AwkValue& right);
			AwkValue evalBinary(Expr& expr);
			AwkValue evalTernary(Expr& expr);
			AwkValue evalArrayRef(Expr& expr);
			AwkValue evalArrayInTest(Expr& expr);
			AwkValue evalAssign(Expr& expr);
			AwkValue evalRegexLiteral(Expr& expr);
			AwkValue applyOp(const AwkValue& current, const std::string& op, const AwkValue& rhs);
			AwkValue assignTo(Expr& lhs, const std::string& op, AwkValue rhs);
			int fieldIndex(Expr& index_expr);
			AwkValue callUser(Expr& call);
			void bindArgument(Expr& arg, Cell& param);
			bool passesByReference(const Expr& arg);
			bool loopBodyDone();
			void runIf(Stmt& stmt);
			void runWhile(Stmt& stmt);
			void runDoWhile(Stmt& stmt);
			void runFor(Stmt& stmt);
			void runForIn(Stmt& stmt);
			void runDelete(Stmt& stmt);
			void runPrint(Stmt& stmt);
			void runPrintf(Stmt& stmt);
			void runBlock(Stmt& stmt);
			void runExit(Stmt& stmt);
			void runReturn(Stmt& stmt);
			std::vector<AwkValue> evalAll(const std::vector<Expr*>& exprs);
			bool patternHolds(Expr& expr);
			bool rangeHolds(AwkRule& rule);
			bool ruleMatches(AwkRule& rule);
			void runRecordRules();
			void runSpecialBlocks(AwkPattern::Kind kind);

			// awk_fields.cpp
			std::vector<std::string> recordParts();
			const std::regex* cachedRegex(CachedRegex& cache, const std::string& pattern);
			std::string paragraphFieldPattern() const;
			void rebuildRecord();
			void growFields(std::size_t count);

			// awk_builtins.cpp
			std::string argText(Expr& call, std::size_t index);
			double argNumber(Expr& call, std::size_t index);
			const std::regex* regexArg(Expr& call, std::size_t index, std::regex& storage);
			AwkValue callLength(Expr& call);
			AwkValue callSubstr(Expr& call);
			AwkValue callIndex(Expr& call);
			AwkValue callCaseConvert(Expr& call);
			std::vector<std::string> splitArgument(Expr& call, const std::string& source);
			AwkValue callSplit(Expr& call);
			AwkValue callSubOrGsub(Expr& call);
			AwkValue callMatch(Expr& call);
			AwkValue callSprintf(Expr& call);
			AwkValue callSystem(Expr& call);
			AwkValue callClose(Expr& call);
			AwkValue callFflush(Expr& call);
			AwkValue callRandom(Expr& call);
			AwkValue callMath(Expr& call);

			// awk_io.cpp
			bool readRecord(InputStream& stream, std::string& out);
			bool readCharRecord(InputStream& stream, char separator, std::string& out);
			bool readParagraphRecord(InputStream& stream, std::string& out);
			bool readRegexRecord(InputStream& stream, std::string& out);
			bool readMainRecord(std::string& out);
			bool openNextMainInput();
			bool openMainOperand(const std::string& operand);
			int readGetlineInput(Expr& expr, std::string& line);
			void storeGetlineResult(Expr& expr, std::string line);
			InputStream* inputFile(const std::string& path);
			InputStream* inputCommand(const std::string& command);
			OutputStream* outputFor(Stmt& stmt);
			OutputStream* outputFile(const std::string& path, bool truncate);
			OutputStream* outputPipe(const std::string& command);
			OutputStream* addOutput(const std::string& key, std::unique_ptr<OutputStream> stream);
			int finishOutput(OutputStream& stream);
			int runOutputPipe(OutputStream& stream);
		};

		/// `name=value` command-line operands (POSIX: an identifier, then '=').
		bool isAssignmentOperand(const std::string& operand);
		void applyAssignmentOperand(Interpreter& interp, const std::string& operand);

	}  // namespace awk_detail

}  // namespace wbsh
