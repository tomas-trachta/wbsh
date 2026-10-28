/**
 * @file awk_io.cpp
 * @brief awk input and output: record reading (RS), the ARGV input list, getline,
 *        output redirections, command pipes, close / fflush / system.
 */

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <io.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "awk_interp.h"
#include "coreutils_internal.h"
#include "executor.h"
#include "pathconv.h"

namespace wbsh {

	namespace awk_detail {

		static const std::size_t kBlockSize = 65536;

		static bool isDiskFile(std::FILE* file) {
			const intptr_t handle = _get_osfhandle(_fileno(file));
			if (handle == -1) return false;
			return ::GetFileType(reinterpret_cast<HANDLE>(handle)) == FILE_TYPE_DISK;
		}

		static std::unique_ptr<InputStream> newInputStream(std::FILE* file) {
			auto stream = std::make_unique<InputStream>();
			stream->file = file;
			stream->owned = file != stdin;
			stream->block_reads = isDiskFile(file);
			return stream;
		}

		// Appends the next block, dropping the consumed prefix of the buffer.
		static bool fillBlock(InputStream& stream) {
			if (stream.file == nullptr) return false;

			stream.buffer.erase(0, stream.start);
			stream.start = 0;
			const std::size_t old_size = stream.buffer.size();
			stream.buffer.resize(old_size + kBlockSize);
			char* destination = &stream.buffer[old_size];
			const std::size_t got = std::fread(destination, 1, kBlockSize, stream.file);
			stream.buffer.resize(old_size + got);
			return got > 0;
		}

		// Called with the buffer used up.
		static bool refill(InputStream& stream) {
			if (stream.block_reads) return fillBlock(stream);
			if (stream.file == nullptr) return false;

			const int c = std::getc(stream.file);
			if (c == EOF) return false;

			stream.buffer.assign(1, static_cast<char>(c));
			stream.start = 0;
			return true;
		}

		static int peekChar(InputStream& stream) {
			if (stream.start >= stream.buffer.size() && !refill(stream)) return EOF;
			return static_cast<unsigned char>(stream.buffer[stream.start]);
		}

		// After a successful takeChar the character is still in the buffer, so
		// `--stream.start` puts it back.
		static int takeChar(InputStream& stream) {
			const int c = peekChar(stream);
			if (c != EOF) ++stream.start;
			return c;
		}

		// Moves buffered text up to @p separator into @p out; true when the
		// separator was found.
		static bool takeBufferedUntil(InputStream& stream, char separator, std::string& out) {
			const std::size_t found = stream.buffer.find(separator, stream.start);
			if (found != std::string::npos) {
				out.append(stream.buffer, stream.start, found - stream.start);
				stream.start = found + 1;
				return true;
			}

			out.append(stream.buffer, stream.start, std::string::npos);
			stream.buffer.clear();
			stream.start = 0;
			return false;
		}

		static bool readFileUntil(std::FILE* file, char separator, std::string& out) {
			bool any = false;
			const int wanted = static_cast<unsigned char>(separator);
			_lock_file(file);
			int c = 0;
			while ((c = _getc_nolock(file)) != EOF) {
				any = true;
				if (c == wanted) break;
				out.push_back(static_cast<char>(c));
			}

			_unlock_file(file);
			return any;
		}

		bool Interpreter::readBlockRecord(InputStream& stream, char separator, std::string& out) {
			std::size_t scan_from = stream.start;
			for (;;) {
				const std::size_t found = stream.buffer.find(separator, scan_from);
				if (found != std::string::npos) {
					out.assign(stream.buffer, stream.start, found - stream.start);
					stream.start = found + 1;
					return true;
				}

				const std::size_t scanned = stream.buffer.size() - stream.start;
				if (!fillBlock(stream)) break;
				scan_from = scanned;
			}

			if (stream.start >= stream.buffer.size()) return false;

			out.assign(stream.buffer, stream.start, std::string::npos);
			stream.start = stream.buffer.size();
			return true;
		}

		bool Interpreter::readCharRecord(InputStream& stream, char separator, std::string& out) {
			if (stream.block_reads) return readBlockRecord(stream, separator, out);

			out.clear();
			const bool had_buffered = stream.start < stream.buffer.size();
			if (had_buffered && takeBufferedUntil(stream, separator, out)) return true;
			if (stream.file == nullptr) return had_buffered;
			return readFileUntil(stream.file, separator, out) || had_buffered;
		}

		// RS = "": records are separated by blank lines; leading and trailing
		// newlines of the input are not part of any record.
		bool Interpreter::readParagraphRecord(InputStream& stream, std::string& out) {
			out.clear();
			while (peekChar(stream) == '\n') ++stream.start;
			if (peekChar(stream) == EOF) return false;

			std::size_t newlines = 0;
			int c = 0;
			while ((c = takeChar(stream)) != EOF) {
				if (c == '\n') {
					++newlines;
					continue;
				}

				if (newlines >= 2) {
					--stream.start;
					break;
				}

				out.append(newlines, '\n');
				newlines = 0;
				out.push_back(static_cast<char>(c));
			}

			return true;
		}

		static void slurpRemaining(InputStream& stream) {
			if (stream.file == nullptr) return;

			std::vector<char> chunk(kBlockSize);
			std::size_t got = 0;
			while ((got = std::fread(chunk.data(), 1, chunk.size(), stream.file)) > 0) {
				stream.buffer.append(chunk.data(), got);
			}
		}

		// The first non-empty match at or after stream.start.
		static bool findSeparator(const InputStream& stream, const AwkRegex& re, std::size_t& pos,
				std::size_t& length) {
			std::size_t from = stream.start;
			while (awkRegexFind(re, stream.buffer, from, pos, length)) {
				if (length > 0) return true;
				from = pos + 1;
			}

			return false;
		}

		// Any other RS is a regular expression (a gawk extension).
		bool Interpreter::readRegexRecord(InputStream& stream, std::string& out) {
			slurpRemaining(stream);
			if (stream.start >= stream.buffer.size()) return false;

			const AwkRegex* re = separatorRegex(rs_regex, RS);
			std::size_t pos = 0;
			std::size_t length = 0;
			if (re == nullptr || !findSeparator(stream, *re, pos, length)) {
				out = stream.buffer.substr(stream.start);
				stream.start = stream.buffer.size();
				return true;
			}

			out = stream.buffer.substr(stream.start, pos - stream.start);
			stream.start = pos + length;
			return true;
		}

		bool Interpreter::readRecord(InputStream& stream, std::string& out) {
			if (RS.size() == 1) return readCharRecord(stream, RS[0], out);
			if (RS.empty()) return readParagraphRecord(stream, out);
			return readRegexRecord(stream, out);
		}

		bool isAssignmentOperand(const std::string& operand) {
			if (operand.empty()) return false;

			const auto first = static_cast<unsigned char>(operand[0]);
			if (!std::isalpha(first) && first != '_') return false;

			for (std::size_t i = 1; i < operand.size(); ++i) {
				const auto c = static_cast<unsigned char>(operand[i]);
				if (c == '=') return true;
				if (!std::isalnum(c) && c != '_') return false;
			}

			return false;
		}

		void applyAssignmentOperand(Interpreter& interp, const std::string& operand) {
			const std::size_t eq = operand.find('=');
			const int slot = interp.globalSlot(operand.substr(0, eq));
			if (slot < 0) return;

			interp.setGlobal(slot, AwkValue::input(expandEscapes(operand.substr(eq + 1))));
		}

		static bool isStdinName(const std::string& name) {
			return name == "-" || name == "/dev/stdin";
		}

		bool Interpreter::openMainOperand(const std::string& operand) {
			std::FILE* file = isStdinName(operand) ? stdin : fopenNative(exec, operand, "rb");
			if (file == nullptr) {
				std::fflush(stdout);
				std::fprintf(stderr, "awk: cannot open: %s\n", operand.c_str());
				input_error = true;
				return false;
			}

			main_input = newInputStream(file);
			setGlobal(kFILENAME, AwkValue::str(operand));
			FNR = 0;
			return true;
		}

		// Walks ARGV[1..ARGC-1] as it is at that moment: BEGIN may have edited
		// it, empty or deleted entries are skipped, `var=value` entries assign.
		bool Interpreter::openNextMainInput() {
			while (next_operand < static_cast<int>(getGlobal(kARGC).asNumber())) {
				const AwkArray& argv = globalArray(kARGV);
				const auto it = argv.find(std::to_string(next_operand++));
				if (it == argv.end()) continue;

				const std::string operand = text(it->second);
				if (operand.empty()) continue;
				if (isAssignmentOperand(operand)) {
					applyAssignmentOperand(*this, operand);
					continue;
				}

				opened_operand = true;
				if (openMainOperand(operand)) return true;
			}

			if (opened_operand) return false;

			opened_operand = true;
			main_input = newInputStream(stdin);
			FNR = 0;
			return true;
		}

		bool Interpreter::readMainRecord(std::string& out) {
			for (;;) {
				if (main_input && readRecord(*main_input, out)) return true;

				main_input.reset();
				if (!openNextMainInput()) return false;
			}
		}

		InputStream* Interpreter::inputFile(const std::string& path) {
			const auto it = input_files.find(path);
			if (it != input_files.end()) return it->second.get();

			std::FILE* file = isStdinName(path) ? stdin : fopenNative(exec, path, "rb");
			if (file == nullptr) return nullptr;

			std::unique_ptr<InputStream> stream = newInputStream(file);
			InputStream* raw = stream.get();
			input_files[path] = std::move(stream);
			return raw;
		}

		// The command runs to completion in a wbsh subshell; its output is then
		// served record by record.
		InputStream* Interpreter::inputCommand(const std::string& command) {
			const auto it = input_commands.find(command);
			if (it != input_commands.end()) return it->second.get();

			flushAllOutput();
			auto stream = std::make_unique<InputStream>();
			stream->buffer = exec.runRaw(command);
			InputStream* raw = stream.get();
			input_commands[command] = std::move(stream);
			return raw;
		}

		int Interpreter::readGetlineInput(Expr& expr, std::string& line) {
			InputStream* stream = nullptr;
			switch (expr.source) {
				case GetlineSource::File:
					stream = inputFile(text(eval(*expr.second)));
					break;
				case GetlineSource::Command:
					stream = inputCommand(text(eval(*expr.third)));
					break;
				default:
					return readMainRecord(line) ? 1 : 0;
			}

			if (stream == nullptr) return -1;
			return readRecord(*stream, line) ? 1 : 0;
		}

		// POSIX: plain getline sets $0 NF NR FNR, `getline var` var NR FNR,
		// `getline <file` $0 NF, `getline var <file` var; `cmd | getline` NR only.
		void Interpreter::storeGetlineResult(Expr& expr, std::string line) {
			if (expr.source != GetlineSource::File) ++NR;
			if (expr.source == GetlineSource::Main) ++FNR;

			if (expr.first != nullptr) {
				assignLValue(*expr.first, AwkValue::input(std::move(line)));
				return;
			}

			setRecord(std::move(line));
		}

		AwkValue Interpreter::doGetline(Expr& expr) {
			std::string line;
			const int status = readGetlineInput(expr, line);
			if (status > 0) storeGetlineResult(expr, std::move(line));
			return AwkValue::num(static_cast<double>(status));
		}

		OutputStream* Interpreter::addOutput(const std::string& key,
				std::unique_ptr<OutputStream> stream) {
			OutputStream* raw = stream.get();
			outputs[key] = std::move(stream);
			output_order.push_back(key);
			return raw;
		}

		OutputStream* Interpreter::outputFile(const std::string& path, bool truncate) {
			const auto it = outputs.find(path);
			if (it != outputs.end()) return it->second.get();

			auto stream = std::make_unique<OutputStream>();
			if (path == "/dev/stdout") {
				stream->file = stdout;
			} else if (path == "/dev/stderr") {
				stream->file = stderr;
			} else {
				stream->file = fopenNative(exec, path, truncate ? "wb" : "ab");
				stream->owned = true;
			}

			if (stream->file == nullptr) {
				runtimeError("can't redirect to \"" + path + "\"");
				return nullptr;
			}

			return addOutput(path, std::move(stream));
		}

		OutputStream* Interpreter::outputPipe(const std::string& command) {
			const auto it = outputs.find(command);
			if (it != outputs.end()) return it->second.get();

			auto stream = std::make_unique<OutputStream>();
			stream->is_pipe = true;
			stream->command = command;
			return addOutput(command, std::move(stream));
		}

		OutputStream* Interpreter::outputFor(Stmt& stmt) {
			const std::string target = text(eval(*stmt.redirect_target));
			switch (stmt.redirect) {
				case Redirect::Truncate: return outputFile(target, true);
				case Redirect::Append:   return outputFile(target, false);
				case Redirect::Pipe:     return outputPipe(target);
				default:                 return nullptr;
			}
		}

		// Output that can no longer reach stdout ends the program the way
		// SIGPIPE ends a POSIX awk, END rules included.
		void Interpreter::emitToStdout(const std::string& out) {
			std::fwrite(out.data(), 1, out.size(), stdout);
			if (!stdoutFailed()) return;

			exiting = true;
			fatal = true;
			exit_status = stdoutFailureStatus();
		}

		void Interpreter::emit(const std::string& out, Stmt& stmt) {
			if (fatal) return;
			if (stmt.redirect == Redirect::None) {
				emitToStdout(out);
				return;
			}

			OutputStream* stream = outputFor(stmt);
			if (stream == nullptr) return;

			if (stream->is_pipe) {
				stream->pending += out;
				return;
			}

			if (stream->file == stderr) std::fflush(stdout);
			std::fwrite(out.data(), 1, out.size(), stream->file);
		}

		void Interpreter::flushAllOutput() {
			std::fflush(stdout);
			for (auto& entry : outputs) {
				if (entry.second->file != nullptr) std::fflush(entry.second->file);
			}
		}

		int Interpreter::runShellCommand(const std::string& command) {
			std::fflush(stdout);
			return exec.executeText("(" + command + "\n)", "awk");
		}

		static std::string makeTempPath() {
			wchar_t dir[MAX_PATH];
			const DWORD length = ::GetTempPathW(MAX_PATH, dir);
			if (length == 0 || length > MAX_PATH) return {};

			wchar_t path[MAX_PATH];
			if (::GetTempFileNameW(dir, L"awk", 0, path) == 0) return {};
			return wideToUtf8(path);
		}

		static std::string singleQuoted(const std::string& text) {
			std::string quoted = "'";
			for (char c : text) {
				if (c == '\'') quoted += "'\\''";
				else quoted.push_back(c);
			}

			return quoted + "'";
		}

		static bool writeWholeFile(const std::string& path, const std::string& contents) {
			std::FILE* file = _wfopen(utf8ToFileWide(path).c_str(), L"wb");
			if (file == nullptr) return false;

			const std::size_t written = std::fwrite(contents.data(), 1, contents.size(), file);
			const bool ok = written == contents.size();
			return std::fclose(file) == 0 && ok;
		}

		// `print | cmd` text is collected and fed to the command, run in a wbsh
		// subshell, once the pipe is closed.
		int Interpreter::runOutputPipe(OutputStream& stream) {
			const std::string temp = makeTempPath();
			if (temp.empty() || !writeWholeFile(temp, stream.pending)) {
				runtimeError("can't create a temporary file for \"" + stream.command + "\"");
				return -1;
			}

			flushAllOutput();
			const std::string redirected = "(" + stream.command + "\n) < " + singleQuoted(temp);
			const int status = exec.executeText(redirected, "awk");
			_wremove(utf8ToFileWide(temp).c_str());
			return status;
		}

		int Interpreter::finishOutput(OutputStream& stream) {
			if (stream.is_pipe) return runOutputPipe(stream);
			if (!stream.owned) return std::fflush(stream.file) == 0 ? 0 : -1;
			return std::fclose(stream.file) == 0 ? 0 : -1;
		}

		static void forgetOutput(std::vector<std::string>& order, const std::string& name) {
			for (auto it = order.begin(); it != order.end(); ++it) {
				if (*it != name) continue;

				order.erase(it);
				return;
			}
		}

		int Interpreter::closeStream(const std::string& name) {
			const auto output = outputs.find(name);
			if (output != outputs.end()) {
				std::unique_ptr<OutputStream> stream = std::move(output->second);
				outputs.erase(output);
				forgetOutput(output_order, name);
				return finishOutput(*stream);
			}

			if (input_files.erase(name) != 0) return 0;
			if (input_commands.erase(name) != 0) return 0;
			return -1;
		}

		void Interpreter::closeAllStreams() {
			const std::vector<std::string> order = output_order;
			for (const auto& name : order) closeStream(name);

			std::fflush(stdout);
			input_files.clear();
			input_commands.clear();
			main_input.reset();
		}

	}  // namespace awk_detail

}  // namespace wbsh
