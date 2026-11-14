/**
 * @file invocation.cpp
 * @brief The shell's own command line, read the way bash reads it.
 */

#include "invocation.h"

#include <cctype>
#include <string>
#include <vector>

#include "base64.h"
#include "environment.h"
#include "pathconv.h"
#include "source.h"

namespace wbsh {

	namespace invocation_detail {

		struct ArgCursor {
			int    argc;
			char** argv;
			int    next;
		};

		struct ParseState {
			Invocation&  out;
			std::string& error;
			bool         command_mode = false;
			bool         stdin_mode   = false;
			bool         has_encoded  = false;
		};

		enum class LongKind {
			Help,
			AgentInfo,
			Version,
			Interactive,
			Run,
			Tokens,
			Expand,
			Ast,
			NoAst,
			Ignored,
			Login,
			NoProfile,
			NoRc,
			RcFile,
			Posix,
			Verbose,
			Encoded,
			Json,
		};

		struct LongOption {
			const char* name;
			LongKind    kind;
		};

	}  // namespace invocation_detail

	using invocation_detail::ArgCursor;
	using invocation_detail::LongKind;
	using invocation_detail::LongOption;
	using invocation_detail::ParseState;

	static const LongOption kLongOptions[] = {
		{ "--help",           LongKind::Help        },
		{ "--agent-info",     LongKind::AgentInfo   },
		{ "--version",        LongKind::Version     },
		{ "--interactive",    LongKind::Interactive },
		{ "--run",            LongKind::Run         },
		{ "--tokens",         LongKind::Tokens      },
		{ "--expand",         LongKind::Expand      },
		{ "--ast",            LongKind::Ast         },
		{ "--no-ast",         LongKind::NoAst       },
		{ "--noediting",      LongKind::Ignored     },
		{ "--debugger",       LongKind::Ignored     },
		{ "--login",          LongKind::Login       },
		{ "--noprofile",      LongKind::NoProfile   },
		{ "--norc",           LongKind::NoRc        },
		{ "--rcfile",         LongKind::RcFile      },
		{ "--init-file",      LongKind::RcFile      },
		{ "--posix",          LongKind::Posix       },
		{ "--verbose",        LongKind::Verbose     },
		{ "--ec",             LongKind::Encoded     },
		{ "--encodedcommand", LongKind::Encoded     },
		{ "--json",           LongKind::Json        },
	};

	static std::string asciiLower(std::string text) {
		for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}

	static bool hasArg(const ArgCursor& cursor) {
		return cursor.next < cursor.argc;
	}

	static std::string takeArg(ArgCursor& cursor) {
		return cursor.argv[cursor.next++];
	}

	static bool fail(ParseState& state, const std::string& message) {
		state.error = message;
		return false;
	}

	static void setAction(Invocation& out, InvocationAction action) {
		if (out.action == InvocationAction::RunScript) out.action = action;
	}

	std::string invokedShellName(const std::string& argv0) {
		const std::size_t slash = argv0.find_last_of("/\\");
		std::string name = slash == std::string::npos ? argv0 : argv0.substr(slash + 1);
		const bool has_exe = name.size() > 4 && asciiLower(name.substr(name.size() - 4)) == ".exe";
		if (has_exe) name.resize(name.size() - 4);

		return name.empty() ? std::string("wbsh") : name;
	}

	static bool isInvokedAsSh(const std::string& shell_name) {
		return asciiLower(shell_name) == "sh";
	}

	static void addPosixMode(ShellStartup& startup) {
		startup.set_words.emplace_back("-o");
		startup.set_words.emplace_back("posix");
	}

	// PowerShell's -EncodedCommand is UTF-16LE; a UTF-8 command never holds a
	// NUL, so one in the second byte tells the two apart.
	static bool looksLikeUtf16Le(const std::vector<unsigned char>& bytes) {
		return bytes.size() >= 2 && bytes.size() % 2 == 0 && bytes[1] == 0;
	}

	static std::string utf16LeToUtf8(const std::vector<unsigned char>& bytes) {
		std::wstring wide;
		wide.reserve(bytes.size() / 2);
		for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
			wide.push_back(static_cast<wchar_t>(bytes[i] | (bytes[i + 1] << 8)));
		}

		if (!wide.empty() && wide.front() == 0xFEFF) wide.erase(wide.begin());
		return wideToUtf8(wide);
	}

	static std::string standardBase64Alphabet(const std::string& text) {
		std::string standard = text;
		if (asciiLower(standard.substr(0, 3)) == "v1:") standard.erase(0, 3);

		for (char& c : standard) {
			if (c == '-') c = '+';
			if (c == '_') c = '/';
		}

		return standard;
	}

	static bool decodeEncodedCommand(const std::string& text, std::string& out_command) {
		bool ok = false;
		const std::vector<unsigned char> bytes = base64Decode(standardBase64Alphabet(text), ok);
		if (!ok) return false;

		if (looksLikeUtf16Le(bytes)) out_command = utf16LeToUtf8(bytes);
		else                         out_command.assign(bytes.begin(), bytes.end());

		normalizeCrlf(out_command);
		return true;
	}

	static bool takeOptionValue(ParseState& state, ArgCursor& cursor, const std::string& name,
			std::string& out_value) {
		if (!hasArg(cursor)) return fail(state, name + ": option requires an argument");

		out_value = takeArg(cursor);
		return true;
	}

	static bool takeEncodedCommand(ParseState& state, ArgCursor& cursor, const std::string& name) {
		std::string text;
		if (!takeOptionValue(state, cursor, name, text)) return false;
		if (!decodeEncodedCommand(text, state.out.command)) {
			return fail(state, name + ": invalid base64");
		}

		state.has_encoded = true;
		return true;
	}

	static void applyModeLongOption(Invocation& out, LongKind kind) {
		switch (kind) {
		case LongKind::Help:        setAction(out, InvocationAction::PrintHelp);      return;
		case LongKind::AgentInfo:   setAction(out, InvocationAction::PrintAgentInfo); return;
		case LongKind::Version:     setAction(out, InvocationAction::PrintVersion);   return;
		case LongKind::Interactive: out.interactive     = true;                       return;
		case LongKind::Run:         out.force_run       = true;                       return;
		case LongKind::Tokens:      out.show_tokens     = true;                       return;
		case LongKind::Expand:      out.show_expansions = true;                       return;
		case LongKind::Ast:         out.show_ast        = true;                       return;
		case LongKind::NoAst:       out.hide_ast        = true;                       return;
		case LongKind::Json:        out.json            = true;                       return;
		default:                                                                      return;
		}
	}

	static bool applyLongOption(ParseState& state, ArgCursor& cursor, const LongOption& option) {
		ShellStartup& startup = state.out.startup;
		switch (option.kind) {
		case LongKind::Login:     startup.login        = true;          return true;
		case LongKind::NoProfile: startup.read_profile = false;         return true;
		case LongKind::NoRc:      startup.read_rc      = false;         return true;
		case LongKind::Verbose:   startup.set_words.emplace_back("-v"); return true;
		case LongKind::Posix:
			addPosixMode(startup);
			return true;
		case LongKind::RcFile:
			return takeOptionValue(state, cursor, option.name, startup.rcfile);
		case LongKind::Encoded:
			return takeEncodedCommand(state, cursor, option.name);
		default:
			applyModeLongOption(state.out, option.kind);
			return true;
		}
	}

	static const LongOption* findLongOption(const std::string& arg) {
		const std::string name = asciiLower(arg);
		for (const LongOption& option : kLongOptions) {
			if (name == option.name) return &option;
		}

		return nullptr;
	}

	static bool parseLongOption(ParseState& state, ArgCursor& cursor, const std::string& arg) {
		const LongOption* option = findLongOption(arg);
		if (option == nullptr) return fail(state, arg + ": invalid option");

		return applyLongOption(state, cursor, *option);
	}

	static bool takeSetOptionName(ParseState& state, ArgCursor& cursor, char sign) {
		const std::string flag = std::string(1, sign) + "o";
		std::string name;
		if (!takeOptionValue(state, cursor, flag, name)) return false;
		if (findSetOption(name) == nullptr) return fail(state, name + ": invalid option name");

		state.out.startup.set_words.push_back(flag);
		state.out.startup.set_words.push_back(name);
		return true;
	}

	static bool takeShoptName(ParseState& state, ArgCursor& cursor, char sign) {
		std::string name;
		if (!takeOptionValue(state, cursor, std::string(1, sign) + "O", name)) return false;
		if (findShopt(name) == nullptr) return fail(state, name + ": invalid shell option name");

		ShellStartup& startup = state.out.startup;
		if (sign == '-') startup.shopts_on.push_back(name);
		else             startup.shopts_off.push_back(name);
		return true;
	}

	static bool addSetFlag(ParseState& state, char sign, char letter) {
		const std::string flag{ sign, letter };
		if (findSetFlag(letter) == nullptr) return fail(state, flag + ": invalid option");

		state.out.startup.set_words.push_back(flag);
		return true;
	}

	static bool applyClusterLetter(ParseState& state, ArgCursor& cursor, char sign, char letter) {
		switch (letter) {
		case 'c': state.command_mode      = true; return true;
		case 's': state.stdin_mode        = true; return true;
		case 'i': state.out.interactive   = true; return true;
		case 'l': state.out.startup.login = true; return true;
		case 'r': state.out.force_run     = true; return true;
		case 'D':                                 return true;
		case 'o': return takeSetOptionName(state, cursor, sign);
		case 'O': return takeShoptName(state, cursor, sign);
		default:  return addSetFlag(state, sign, letter);
		}
	}

	// A lone -h has always been wbsh's help; inside a cluster it is bash's hashall.
	static bool parseCluster(ParseState& state, ArgCursor& cursor, const std::string& arg) {
		if (arg == "-h") {
			setAction(state.out, InvocationAction::PrintHelp);
			return true;
		}

		for (std::size_t i = 1; i < arg.size(); ++i) {
			if (!applyClusterLetter(state, cursor, arg[0], arg[i])) return false;
		}

		return true;
	}

	static bool isOptionWord(const std::string& arg) {
		return arg.size() > 1 && (arg[0] == '-' || arg[0] == '+');
	}

	static bool parseOptionWord(ParseState& state, ArgCursor& cursor, const std::string& arg) {
		if (arg.compare(0, 2, "--") == 0) return parseLongOption(state, cursor, arg);

		return parseCluster(state, cursor, arg);
	}

	static bool parseOptions(ParseState& state, ArgCursor& cursor) {
		while (hasArg(cursor)) {
			const std::string arg = cursor.argv[cursor.next];
			if (arg == "--" || arg == "-") {
				++cursor.next;
				return true;
			}

			if (!isOptionWord(arg)) return true;

			++cursor.next;
			if (!parseOptionWord(state, cursor, arg)) return false;
		}

		return true;
	}

	static void splitFirstOperand(Invocation& out, const std::vector<std::string>& operands,
			std::string& out_first) {
		out_first = operands.front();
		out.operands.assign(operands.begin() + 1, operands.end());
	}

	static bool assignSource(ParseState& state, const ArgCursor& cursor) {
		Invocation& out = state.out;
		std::vector<std::string> operands(cursor.argv + cursor.next, cursor.argv + cursor.argc);

		if (state.has_encoded) {
			out.source = InvocationSource::CommandString;
			out.operands = std::move(operands);
			return true;
		}

		if (state.command_mode) {
			if (operands.empty()) return fail(state, "-c: option requires an argument");

			out.source = InvocationSource::CommandString;
			splitFirstOperand(out, operands, out.command);
			return true;
		}

		if (state.stdin_mode || operands.empty()) {
			out.source = state.stdin_mode ? InvocationSource::Stdin : InvocationSource::Unset;
			out.operands = std::move(operands);
			return true;
		}

		out.source = InvocationSource::File;
		splitFirstOperand(out, operands, out.file_path);
		return true;
	}

	bool parseInvocation(int argc, char** argv, Invocation& out, std::string& out_error) {
		ParseState state{ out, out_error };
		ArgCursor cursor{ argc, argv, 1 };
		if (argc > 0) out.shell_name = invokedShellName(argv[0]);
		if (isInvokedAsSh(out.shell_name)) addPosixMode(out.startup);

		if (!parseOptions(state, cursor)) return false;

		return assignSource(state, cursor);
	}

	bool invocationExecutes(const Invocation& invocation) {
		const bool dumping = invocation.show_tokens || invocation.show_ast
			|| invocation.show_expansions;
		return invocation.force_run || !dumping;
	}

	bool invocationShowsAst(const Invocation& invocation) {
		if (invocation.show_ast) return true;
		if (invocation.hide_ast) return false;

		return !invocationExecutes(invocation);
	}

}  // namespace wbsh
