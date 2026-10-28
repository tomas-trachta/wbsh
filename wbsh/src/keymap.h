#pragma once

/**
 * @file keymap.h
 * @brief Readline's key bindings for the line editor: the emacs keymap,
 *        ~/.inputrc, and the `bind` builtin.
 *
 * A key is the byte sequence a terminal sends for it: "\x01" is Ctrl-A,
 * "\x1b" "b" is Meta-b, "\x1b[A" is Up. A binding runs one of the editor's
 * named commands, types a macro, or runs a shell command (`bind -x`).
 */

#include <map>
#include <string>
#include <vector>

namespace wbsh {

	class Executor;

	struct KeyBinding {
		enum class Kind {
			kCommand,        // `value` names an editing command
			kMacro,          // `value` is typed as if it were keys
			kShellCommand,   // `value` runs with READLINE_LINE / READLINE_POINT
		};
		Kind kind = Kind::kCommand;
		std::string value;
	};

	enum class KeyLookup {
		kUnbound,   // no binding starts with these keys
		kPrefix,    // the start of a longer binding: wait for the next key
		kBound,     // a binding for exactly these keys
	};

	/** The shell's bindings and readline variables, starting from readline's emacs keymap. */
	class Keymap {
	public:
		Keymap();

		KeyLookup lookup(const std::string& keys, KeyBinding& binding) const;
		void bind(const std::string& keys, const KeyBinding& binding);
		void unbind(const std::string& keys);
		const std::map<std::string, KeyBinding>& bindings() const { return bindings_; }

		void setVariable(const std::string& name, const std::string& value);
		std::string variable(const std::string& name) const;
		bool isOn(const std::string& name) const;
		const std::map<std::string, std::string>& variables() const { return variables_; }

	private:
		std::map<std::string, KeyBinding> bindings_;
		std::map<std::string, std::string> variables_;
	};

	Keymap& shellKeymap();

	/** The editing commands a key can be bound to, as `bind -l` lists them. */
	const std::vector<std::string>& editingCommandNames();

	/** Reads $INPUTRC, else ~/.inputrc, into the shell's keymap; quietly nothing without one. */
	void loadInputrc(Executor& exec);

	/** One inputrc line (`"\C-t": transpose-chars`, `set bell-style none`); false with @p error. */
	bool applyInputrcLine(const std::string& line, std::string& error);

	/**
	 * The shell's changes to the default keymap as `bind` commands, which a
	 * self-spawned subshell runs so that it sees the same bindings.
	 */
	std::string serializeKeyBindings();

	/** `bind`: list, change and read key bindings. */
	int bindBuiltin(Executor& exec, const std::vector<std::string>& args);

}  // namespace wbsh
