#pragma once

/**
 * @file environment.h
 * @brief Shell variable, array, and parameter store.
 *
 * Holds named variables, the export set, positional parameters
 * (`$1`, `$2`, …), shell options (`set -e`, `set -u`, …), shopt
 * flags, and the dynamic parameters (`$RANDOM`, `$SECONDS`,
 * `$LINENO`, `$?`, `$$`, `$!`, `$-`). One Environment per shell.
 *
 * A name lives in at most one of {scalar, indexed array, assoc
 * array}; assigning across forms drops the old form.
 */

#include <chrono>
#include <ctime>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assocmap.h"

namespace wbsh {

	/// The `set -o` options. Those wbsh has no counterpart for (monitor,
	/// posix, ...) are only stored, so scripts that set them
	/// keep working and `set -o` reports them back.
	struct SetOptions {
		bool allexport   = false;
		bool braceexpand = true;
		bool emacs       = false;
		bool errexit     = false;
		bool errtrace    = false;
		bool functrace   = false;
		bool hashall     = true;
		bool histexpand  = false;
		bool history     = false;
		bool ignoreeof   = false;
		bool interactive_comments = true;
		bool keyword     = false;
		bool monitor     = false;
		bool noclobber   = false;
		bool noexec      = false;
		bool noglob      = false;
		bool nolog       = false;
		bool notify      = false;
		bool nounset     = false;
		bool onecmd      = false;
		bool physical    = false;
		bool pipefail    = false;
		bool posix       = false;
		bool privileged  = false;
		bool verbose     = false;
		bool vi          = false;
		bool xtrace      = false;
	};

	/// Every `shopt` flag bash 5.3 has, so rc files written for bash load
	/// without errors; not all of them change behaviour yet.
	struct ShoptFlags {
		/// Also what `assoc_expand_once`, bash 5.3's deprecated synonym, sets.
		bool array_expand_once       = false;
		bool autocd                  = false;
		bool bash_source_fullpath    = false;
		bool cdable_vars             = false;
		bool cdspell                 = false;
		bool checkhash               = false;
		bool checkjobs               = false;
		bool checkwinsize            = true;
		bool cmdhist                 = true;
		bool compat31                = false;
		bool compat32                = false;
		bool compat40                = false;
		bool compat41                = false;
		bool compat42                = false;
		bool compat43                = false;
		bool compat44                = false;
		bool complete_fullquote      = true;
		bool direxpand               = false;
		bool dirspell                = false;
		bool dotglob                 = false;
		bool execfail                = false;
		bool expand_aliases          = false;
		bool extdebug                = false;
		bool extglob                 = false;
		bool extquote                = true;
		bool failglob                = false;
		bool force_fignore           = true;
		bool globasciiranges         = true;
		bool globskipdots            = true;
		bool globstar                = false;
		bool gnu_errfmt              = false;
		bool histappend              = false;
		bool histreedit              = false;
		bool histverify              = false;
		bool hostcomplete            = true;
		bool huponexit               = false;
		bool inherit_errexit         = false;
		bool interactive_comments    = true;
		bool lastpipe                = false;
		bool lithist                 = false;
		bool localvar_inherit        = false;
		bool localvar_unset          = false;
		bool login_shell             = false;
		bool mailwarn                = false;
		bool no_empty_cmd_completion = false;
		bool nocaseglob              = false;
		bool nocasematch             = false;
		bool noexpand_translation    = false;
		bool nullglob                = false;
		bool patsub_replacement      = true;
		bool progcomp                = true;
		bool progcomp_alias          = false;
		bool promptvars              = true;
		bool restricted_shell        = false;
		bool shift_verbose           = false;
		bool sourcepath              = true;
		bool varredir_close          = false;
		bool xpg_echo                = false;
	};

	struct ShoptSpec {
		const char* name;
		bool ShoptFlags::* flag;
	};

	/// Every `shopt` flag, in the order `shopt` lists them.
	const std::vector<ShoptSpec>& shoptTable();
	const ShoptSpec* findShopt(const std::string& name);

	struct SetOptionSpec {
		const char* name;
		char        letter;
		bool SetOptions::* flag;
	};

	/// Every `set -o` option, sorted by name; `letter` is 0 for long-only ones.
	const std::vector<SetOptionSpec>& setOptionTable();
	const SetOptionSpec* findSetOption(const std::string& name);
	const SetOptionSpec* findSetFlag(char letter);

	/// The `declare` attributes that transform a value as it is stored.
	enum VarAttribute : unsigned {
		kAttrInteger   = 1u << 0,
		kAttrLowercase = 1u << 1,
		kAttrUppercase = 1u << 2,
		kAttrCapitalize = 1u << 3,
	};

	class Environment {
	public:
		Environment();

		/// Assign @p value to @p name (respects readonly, may unset arrays).
		void   set(const std::string& name, std::string value);
		/**
		 * @brief Bulk write that bypasses the readonly check.
		 *
		 * Used by internal state-restoration paths (subshell
		 * teardown, function-call locals) where we're undoing
		 * changes the user couldn't have made directly.
		 */
		void   forceSet(const std::string& name, std::string value) {
			vars_[name] = std::move(value);
		}
		bool   unset(const std::string& name);
		/// Erase one array element: `assoc[key]` if @p name is an assoc
		/// array, else `indexed[idx]`. No-op if @p name isn't an array.
		void   unsetElement(const std::string& name, long long idx, const std::string& key);
		bool   has(const std::string& name) const;
		std::string get(const std::string& name) const;
		/// The field separators: `$IFS`, or `<space><tab><newline>` when
		/// IFS is unset (POSIX). A set but empty IFS stays empty.
		std::string ifs() const;

		void   exportVar(const std::string& name);
		void   unexportVar(const std::string& name);
		bool   isExported(const std::string& name) const;
		/// An exported entry by its exact name, which need not be one a
		/// variable can have (`v[0]` from the environment the shell got).
		bool   exportedEntry(const std::string& name, std::string& out) const;
		const std::unordered_set<std::string>& exportedSet() const { return exported_; }
		/// Mark @p name as readonly; further set() calls will fail.
		void   markReadonly(const std::string& name)   { readonly_.insert(resolveRef(name)); }
		bool   isReadonly(const std::string& name) const {
			return readonly_.count(resolveRef(name)) != 0;
		}
		const std::unordered_set<std::string>& readonlySet() const { return readonly_; }
		void   clearReadonly(const std::string& name) { readonly_.erase(name); }

		unsigned attributes(const std::string& name) const;
		/// The attribute letters `declare -p` and `${x@a}` show, in bash's order.
		std::string attributeLetters(const std::string& name) const;
		void     setAttributes(const std::string& name, unsigned attributes);
		const std::unordered_map<std::string, unsigned>& attributeMap() const {
			return attributes_;
		}
		/// The value `name+=suffix` stores: a sum for an integer variable.
		std::string appendedValue(const std::string& name, const std::string& suffix) const;
		/// `+=` onto @p current, an element of @p name: arithmetic under `-i`.
		std::string appendedTo(const std::string& name, const std::string& current,
		                       const std::string& suffix) const;

		/// Evaluates the values assigned to `declare -i` variables; the
		/// Executor installs its arithmetic evaluator here.
		using ArithEvaluator = bool (*)(void* context, const std::string& expr, long long& out);
		void setArithEvaluator(ArithEvaluator evaluator, void* context) {
			arith_evaluator_ = evaluator;
			arith_context_ = context;
		}
		/// Evaluates the subscript of a `name[sub]` that a builtin was given.
		void setSubscriptEvaluator(ArithEvaluator evaluator) { subscript_evaluator_ = evaluator; }

		const std::unordered_map<std::string, std::string>& vars() const { return vars_; }

		/**
		 * @name Name references (`declare -n`)
		 *
		 * Every value and attribute accessor follows a nameref to the
		 * variable it names (possibly an array element, `a[1]`); these
		 * work on the nameref itself. A nameref with an empty target
		 * stands for nothing until a value is assigned to it, which
		 * becomes its target.
		 */
		///@{
		bool isNameref(const std::string& name) const { return namerefs_.count(name) != 0; }
		/// The raw target of nameref @p name; empty for an unset or plain variable.
		std::string namerefTarget(const std::string& name) const;
		/// Make @p name a nameref to @p target, dropping any value it held.
		void setNameref(const std::string& name, std::string target);
		/// `declare +n`: @p name becomes a plain variable holding the target name.
		void clearNameref(const std::string& name);
		/// `unset -n`: remove @p name itself, nameref or not.
		void unsetNameref(const std::string& name);
		const std::unordered_map<std::string, std::string>& namerefs() const {
			return namerefs_;
		}
		/// The variable @p name stands for after following namerefs.
		const std::string& resolveRef(const std::string& name) const;
		/// A value for a nameref without a target must name a variable;
		/// reports one that does not.
		bool rejectsRefTarget(const std::string& name, const std::string& value) const;
		bool rejectsRefValue(const std::string& value) const;
		void dropUnboundNameref(const std::string& name);
		/// The builtin whose assignments are running, for their errors.
		void setAssigningCommand(std::string name) { assigning_command_ = std::move(name); }
		const std::string& assigningCommand() const { return assigning_command_; }
		///@}

		/// Sparse indexed array — bash arrays are sparse, so a sorted map.
		using IndexedArray = std::map<long long, std::string>;
		using AssocArray   = BashAssocMap;

		/// Everything a function's `local` shadows, so the return can put it back.
		struct VarState {
			bool has_scalar  = false;
			bool has_indexed = false;
			bool has_assoc   = false;
			std::string  scalar;
			IndexedArray indexed;
			AssocArray   assoc;
			bool exported = false;
			bool readonly = false;
			unsigned attributes = 0;
			bool has_nameref = false;
			std::string nameref;
			bool valueless = false;
		};
		VarState saveVar(const std::string& name) const;
		/// Put @p name back as saved, readonly or not.
		void     restoreVar(const std::string& name, VarState state);

		bool isIndexedArray(const std::string& name) const {
			return indexed_.count(resolveRef(name)) != 0;
		}
		bool isAssocArray(const std::string& name) const {
			return assoc_.count(resolveRef(name)) != 0;
		}
		const IndexedArray* getIndexedArray(const std::string& name) const {
			auto it = indexed_.find(resolveRef(name));
			return it == indexed_.end() ? nullptr : &it->second;
		}
		/// For in-place edits by the shell itself: null unless @p name is
		/// an indexed array with no nameref, attribute, readonly flag or
		/// scalar, so an edit means the same as setIndexedArrayFromList.
		IndexedArray* plainIndexedArray(const std::string& name) {
			const auto found = indexed_.find(name);
			if (found == indexed_.end()) return nullptr;
			if (!namerefs_.empty() && namerefs_.count(name) != 0) return nullptr;
			if (!valueless_.empty() && valueless_.count(name) != 0) return nullptr;
			if (readonly_.count(name) != 0 || vars_.count(name) != 0) return nullptr;

			const auto attrs = attributes_.find(name);
			return attrs != attributes_.end() && attrs->second != 0 ? nullptr : &found->second;
		}
		const AssocArray* getAssocArray(const std::string& name) const {
			auto it = assoc_.find(resolveRef(name));
			return it == assoc_.end() ? nullptr : &it->second;
		}
		void setIndexedArrayFromList(const std::string& name,
		                             std::vector<std::string> values);
		void setIndexedArraySparse(const std::string& name,
		                           std::map<long long, std::string> elems);
		/**
		 * @brief Set `arr[idx] = val`.
		 *
		 * If @p name was scalar, it is promoted to an indexed array
		 * with the prior scalar at index 0 plus this assignment.
		 */
		void setIndexedElement(const std::string& name, long long idx, std::string val);
		/// A negative @p idx counts back from one past the highest index;
		/// false, reported, when it still ends up below zero.
		bool resolveElementIndex(const std::string& name, long long& idx) const;
		void declareAssocArray(const std::string& name, bool converted = false);
		/// Gives an empty associative array a hash table of @p buckets.
		void sizeAssocTable(const std::string& name, std::uint32_t buckets);
		/// `A=(...)` on an associative array: it starts out empty again.
		void resetAssocArray(const std::string& name);
		/// `declare -a x` / `declare -A x` with no value: the array exists but
		/// was never assigned, which `declare -p` shows without `=()`.
		void markValueless(const std::string& name) { valueless_.insert(resolveRef(name)); }
		void markValued(const std::string& name) { valueless_.erase(resolveRef(name)); }
		unsigned long long optindWrites() const { return optind_writes_; }
		bool isValueless(const std::string& name) const {
			return valueless_.count(resolveRef(name)) != 0;
		}
		/// Set `assoc[key]=val`; promotes @p name to an assoc array if needed.
		void setAssocElement(const std::string& name, std::string key, std::string val);
		/// Drop any array form for @p name (scalar value, if any, is preserved).
		void unsetArray(const std::string& name) {
			indexed_.erase(resolveRef(name));
			assoc_.erase(resolveRef(name));
		}
		const std::unordered_map<std::string, IndexedArray>& indexedArrays() const {
			return indexed_;
		}
		const std::unordered_map<std::string, AssocArray>& assocArrays() const {
			return assoc_;
		}

		void   setPositional(std::vector<std::string> args);
		const std::vector<std::string>& positional() const { return positional_; }

		void   setLastStatus(int s) { last_status_ = s; }
		int    lastStatus() const   { return last_status_; }

		void   setLastBgPid(long long p) { last_bg_pid_ = p; }
		long long lastBgPid() const      { return last_bg_pid_; }

		long long shellPid() const { return shell_pid_; }
		/// A self-spawned pipeline stage reports its parent shell's `$$`.
		void setShellPid(long long pid) { shell_pid_ = pid; }

		/// `$BASHPID`: this process's PID, or the value an in-process
		/// subshell was given.
		long long bashPid() const { return bash_pid_; }
		/**
		 * @brief Give an in-process subshell a `$BASHPID` of its own.
		 *
		 * There is no fork, so the value is synthetic: a serial above
		 * 32 bits joined with the real PID. It is unique within this
		 * process and across wbsh processes, and no real PID can equal it.
		 */
		void enterSubshellPid();
		/// `$BASH_SUBSHELL`: how many subshells deep this shell runs.
		int subshellLevel() const { return subshell_level_; }
		void setSubshellLevel(int level) { subshell_level_ = level; }

		/// `$-`: the single-letter options that are on, in bash's order.
		std::string shellOptions() const;
		std::string enabledSetOptionNames() const;   ///< `$SHELLOPTS`
		std::string enabledShoptNames() const;       ///< `$BASHOPTS`

		SetOptions&       options()       { return options_; }
		const SetOptions& options() const { return options_; }

		bool errexit()     const { return options_.errexit; }
		void setErrexit(bool b)  { options_.errexit = b; }
		bool nounset()     const { return options_.nounset; }
		void setNounset(bool b)  { options_.nounset = b; }
		bool xtrace()      const { return options_.xtrace; }
		void setXtrace(bool b)   { options_.xtrace = b; }
		bool pipefail()    const { return options_.pipefail; }
		void setPipefail(bool b) { options_.pipefail = b; }
		bool noglob()      const { return options_.noglob; }
		void setNoglob(bool b)   { options_.noglob = b; }
		bool allexport()   const { return options_.allexport; }
		bool noclobber()   const { return options_.noclobber; }
		bool noexec()      const { return options_.noexec; }
		bool verbose()     const { return options_.verbose; }
		bool braceexpand() const { return options_.braceexpand; }

		bool interactive() const { return interactive_; }
		/// `c` or `s`: how the shell got its commands, shown last in `$-`.
		void setInvocationLetter(char letter) { invocation_letter_ = letter; }
		/// `set -o posix`: also sets POSIXLY_CORRECT and the shopts bash's
		/// POSIX mode turns on, and puts them back when it is turned off.
		void setPosixMode(bool on);
		void enterRestrictedMode();
		bool restricted() const { return shopt_.restricted_shell; }
		char invocationLetter() const { return invocation_letter_; }
		void setInteractive(bool b);

		/// `shopt` flags. Defaults match bash's interactive defaults where
		/// they line up with POSIX expectations.
		ShoptFlags&       shopts()       { return shopt_; }
		const ShoptFlags& shopts() const { return shopt_; }
		bool nullglob()    const { return shopt_.nullglob; }
		void setNullglob(bool b) { shopt_.nullglob = b; }
		bool dotglob()     const { return shopt_.dotglob; }
		void setDotglob(bool b) { shopt_.dotglob = b; }
		bool extglob()     const { return shopt_.extglob; }
		void setExtglob(bool b) { shopt_.extglob = b; }
		bool nocaseglob()  const { return shopt_.nocaseglob; }
		void setNocaseglob(bool b) { shopt_.nocaseglob = b; }
		bool nocasematch() const { return shopt_.nocasematch; }
		void setNocasematch(bool b) { shopt_.nocasematch = b; }
		bool globstar()    const { return shopt_.globstar; }
		void setGlobstar(bool b) { shopt_.globstar = b; }
		bool lastpipe()    const { return shopt_.lastpipe; }
		void setLastpipe(bool b) { shopt_.lastpipe = b; }
		bool huponexit()   const { return shopt_.huponexit; }
		void setHuponexit(bool b) { shopt_.huponexit = b; }
		bool expand_aliases() const { return shopt_.expand_aliases; }
		void setExpandAliases(bool b) { shopt_.expand_aliases = b; }
		bool autocd()      const { return shopt_.autocd; }
		void setAutocd(bool b) { shopt_.autocd = b; }
		bool checkwinsize() const { return shopt_.checkwinsize; }
		void setCheckwinsize(bool b) { shopt_.checkwinsize = b; }

		/// Whether LC_ALL, else LC_CTYPE, else LANG names a UTF-8 locale;
		/// with none of them set wbsh speaks UTF-8.
		bool utf8Locale() const;
		/// The locale LC_ALL, LC_NUMERIC or LANG names, in that order.
		std::string numericLocale() const;
		/// The locale LC_ALL, LC_CTYPE or LANG names, in that order.
		std::string ctypeLocale() const;

		void   setShellName(std::string s) { shell_name_ = std::move(s); }
		const std::string& shellName() const { return shell_name_; }

		void   loadFromProcessEnv();

		/**
		 * @brief `$RANDOM` — advance the LCG and return the next value.
		 *
		 * Assigning to `RANDOM` (handled in set()) seeds the LCG
		 * without storing the value in `vars_`.
		 */
		unsigned int randomNext();
		void         setRandomSeed(unsigned int s) { random_state_ = s; }
		/// A subshell's `$RANDOM` must not repeat the parent's sequence.
		void         reseedRandom();

		/**
		 * @brief Put back everything a subshell may have changed, from a
		 *        copy taken when it started.
		 *
		 * Variables and arrays the subshell left alone keep their storage,
		 * so references the caller still holds into them stay valid. `$?`
		 * is left for the caller to set from the subshell's status.
		 */
		void         restoreFrom(const Environment& snapshot);

		/**
		 * @brief `$SECONDS` — whole seconds since this Environment was
		 *        constructed, offset by any user assignment.
		 */
		long long    secondsSinceStart() const;
		/// The wall-clock time this Environment was constructed (`printf '%(...)T' -2`).
		std::time_t  startEpoch() const { return start_epoch_; }
		void         setSecondsOffset(long long s);

		int          currentLineno() const   { return current_lineno_; }
		void         setCurrentLineno(int n) { current_lineno_ = n; }
		/// `$BASH_COMMAND`: the command running or about to run.
		const std::string& currentCommand() const { return current_command_; }
		void         setCurrentCommand(std::string text) { current_command_ = std::move(text); }

	private:
		bool rejectIfReadonly(const std::string& name) const;
		bool applyAttributes(const std::string& name, std::string& value) const;

		struct ElementRef {
			std::string base;
			std::string key;
			long long index = 0;
			bool assoc = false;
			bool bad_index = false;   ///< The subscript failed to evaluate.
			bool whole = false;       ///< `[@]` or `[*]`: every element, as a nameref reads it.
		};
		bool        splitElementRef(const std::string& ref, ElementRef& out) const;
		std::string getElement(const ElementRef& element) const;
		std::string wholeArrayText(const ElementRef& element) const;
		bool        hasElement(const ElementRef& element) const;
		void        setResolved(const std::string& name, std::string value);
		std::string getResolved(const std::string& name) const;
		bool        hasResolved(const std::string& name) const;
		long long elapsedSeconds() const;
		void noteGlobIgnore();
		void notePosixlyCorrect();

		struct PosixSavedShopts {
			bool expand_aliases = false;
			bool inherit_errexit = false;
			bool shift_verbose = false;
			bool interactive_comments = true;
		};
		PosixSavedShopts posix_saved_;
		void importProcessVariable(std::string name, std::string value);

		std::unordered_map<std::string, std::string> vars_;
		std::unordered_map<std::string, IndexedArray> indexed_;
		std::unordered_map<std::string, AssocArray> assoc_;
		unsigned long long optind_writes_ = 0;
		std::unordered_set<std::string> exported_;
		std::unordered_set<std::string> readonly_;
		std::unordered_map<std::string, unsigned> attributes_;
		std::unordered_map<std::string, std::string> namerefs_;
		std::unordered_set<std::string> valueless_;
		ArithEvaluator arith_evaluator_ = nullptr;
		ArithEvaluator subscript_evaluator_ = nullptr;
		void* arith_context_ = nullptr;
		std::vector<std::string> positional_;
		int last_status_ = 0;
		long long last_bg_pid_ = 0;
		long long shell_pid_ = 0;
		long long bash_pid_ = 0;
		std::string shell_name_ = "wbsh";
		std::string assigning_command_;
		int subshell_level_ = 0;
		SetOptions options_;
		bool interactive_ = false;
		char invocation_letter_ = 0;
		ShoptFlags shopt_;
		std::chrono::steady_clock::time_point start_time_ =
		    std::chrono::steady_clock::now();
		long long seconds_offset_ = 0;
		std::time_t start_epoch_ = std::time(nullptr);
		unsigned int random_state_ = 0;
		int current_lineno_ = 0;
		std::string current_command_;
	};

	/// The `]` that closes the subscript opened at @p open, as bash's
	/// skipsubscript finds it: nested brackets balance, and unless the
	/// subscript is taken as it is, quoted text hides brackets. npos when
	/// nothing closes it.
	std::size_t subscriptEnd(const std::string& text, std::size_t open, bool skips_quotes);

	/// Under assoc_expand_once, `name[key]` names an element of the
	/// associative array `name` whatever brackets the key holds: it runs
	/// to the `]` that ends @p text, as in bash.
	bool isAssocKeyReference(const Environment& env, const std::string& text);

}  // namespace wbsh
