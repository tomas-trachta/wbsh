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
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wbsh {

	/**
	 * @brief Shell variable & parameter store.
	 *
	 * Owns the in-memory tables for variables, exports, readonly
	 * marks, indexed and associative arrays, positional parameters,
	 * and the shell-option flags.
	 */
	class Environment {
	public:
		Environment();

		// ---- Named variables ----
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
		/// Remove @p name from the variable table (and any export mark).
		void   unset(const std::string& name);
		/// True iff @p name has a scalar value set.
		bool   has(const std::string& name) const;
		/// Scalar value of @p name, or the empty string when unset.
		std::string get(const std::string& name) const;

		/// Mark @p name as exported (visible to spawned processes).
		void   exportVar(const std::string& name);
		/// Drop the export mark on @p name (without unsetting).
		void   unexportVar(const std::string& name);
		/// True iff @p name is currently in the export set.
		bool   isExported(const std::string& name) const;
		/// Mark @p name as readonly; further set() calls will fail.
		void   markReadonly(const std::string& name)   { readonly_.insert(name); }
		/// True iff @p name is marked readonly.
		bool   isReadonly(const std::string& name) const { return readonly_.count(name) != 0; }
		/// Direct access to the readonly set (for `readonly` / `set` output).
		const std::unordered_set<std::string>& readonlySet() const { return readonly_; }

		/// Iterate variables (used by `set` / diagnostics output).
		const std::unordered_map<std::string, std::string>& vars() const { return vars_; }

		// ---- Indexed / associative arrays ----
		/// Sparse indexed array — bash arrays are sparse, so a sorted map.
		using IndexedArray = std::map<long long, std::string>;
		/// Associative array — string keys.
		using AssocArray   = std::map<std::string, std::string>;

		/// True iff @p name is an indexed array.
		bool isIndexedArray(const std::string& name) const { return indexed_.count(name) != 0; }
		/// True iff @p name is an associative array.
		bool isAssocArray(const std::string& name) const   { return assoc_.count(name) != 0; }
		/// Borrowed pointer to the indexed array, or null.
		const IndexedArray* getIndexedArray(const std::string& name) const {
			auto it = indexed_.find(name);
			return it == indexed_.end() ? nullptr : &it->second;
		}
		/// Borrowed pointer to the associative array, or null.
		const AssocArray* getAssocArray(const std::string& name) const {
			auto it = assoc_.find(name);
			return it == assoc_.end() ? nullptr : &it->second;
		}
		/**
		 * @brief Replace @p name with a fresh indexed array.
		 *
		 * Items are assigned at indices `0..values.size()-1`. Drops
		 * any prior scalar or associative form for @p name.
		 */
		void setIndexedArrayFromList(const std::string& name,
		                             std::vector<std::string> values);
		/**
		 * @brief Replace @p name with a sparse indexed array.
		 *
		 * Drops any prior scalar or associative form for @p name.
		 */
		void setIndexedArraySparse(const std::string& name,
		                           std::map<long long, std::string> elems);
		/**
		 * @brief Set `arr[idx] = val`.
		 *
		 * If @p name was scalar, it is promoted to an indexed array
		 * with the prior scalar at index 0 plus this assignment.
		 */
		void setIndexedElement(const std::string& name, long long idx, std::string val);
		/// Declare an empty associative array (so future `assoc[k]=v` works).
		void declareAssocArray(const std::string& name);
		/// Set `assoc[key]=val`; promotes @p name to an assoc array if needed.
		void setAssocElement(const std::string& name, std::string key, std::string val);
		/// Drop any array form for @p name (scalar value, if any, is preserved).
		void unsetArray(const std::string& name) {
			indexed_.erase(name);
			assoc_.erase(name);
		}
		/// Iterate indexed arrays (for `set` / `declare -p`).
		const std::unordered_map<std::string, IndexedArray>& indexedArrays() const { return indexed_; }
		/// Iterate associative arrays (for `set` / `declare -p`).
		const std::unordered_map<std::string, AssocArray>&   assocArrays()   const { return assoc_;   }

		// ---- Positional parameters ----
		/// Replace `$1..$N` with @p args.
		void   setPositional(std::vector<std::string> args);
		/// Read-only access to the positional parameter vector.
		const std::vector<std::string>& positional() const { return positional_; }

		// ---- Special parameters ----
		/// Update `$?` (the last command's exit status).
		void   setLastStatus(int s) { last_status_ = s; }
		/// Current value of `$?`.
		int    lastStatus() const   { return last_status_; }

		/// Update `$!` (last background PID).
		void   setLastBgPid(long long p) { last_bg_pid_ = p; }
		/// Current value of `$!`.
		long long lastBgPid() const      { return last_bg_pid_; }

		/// `$$` — the shell process's PID (captured at construction).
		long long shellPid() const { return shell_pid_; }

		/// `$-` — shell options as a flag-letter string, rebuilt on demand.
		std::string shellOptions() const {
			std::string s;
			if (errexit_) s += 'e';
			if (nounset_) s += 'u';
			if (xtrace_)  s += 'x';
			if (noglob_)  s += 'f';
			return s;
		}

		bool errexit() const  { return errexit_; }   void setErrexit(bool b)  { errexit_ = b; }
		bool nounset() const  { return nounset_; }   void setNounset(bool b)  { nounset_ = b; }
		bool xtrace()  const  { return xtrace_; }    void setXtrace(bool b)   { xtrace_  = b; }
		bool pipefail() const { return pipefail_; }  void setPipefail(bool b) { pipefail_ = b; }
		bool noglob()  const  { return noglob_; }    void setNoglob(bool b)   { noglob_  = b; }

		/// `shopt` flags. Defaults match bash's interactive defaults where
		/// they line up with POSIX expectations.
		bool nullglob()    const { return nullglob_; }    void setNullglob(bool b)    { nullglob_ = b; }
		bool dotglob()     const { return dotglob_; }     void setDotglob(bool b)     { dotglob_ = b; }
		bool extglob()     const { return extglob_; }     void setExtglob(bool b)     { extglob_ = b; }
		bool nocaseglob()  const { return nocaseglob_; }  void setNocaseglob(bool b)  { nocaseglob_ = b; }
		bool nocasematch() const { return nocasematch_; } void setNocasematch(bool b) { nocasematch_ = b; }
		bool globstar()    const { return globstar_; }    void setGlobstar(bool b)    { globstar_ = b; }
		bool lastpipe()    const { return lastpipe_; }    void setLastpipe(bool b)    { lastpipe_ = b; }
		bool huponexit()   const { return huponexit_; }   void setHuponexit(bool b)   { huponexit_ = b; }
		bool expand_aliases() const { return expand_aliases_; } void setExpandAliases(bool b) { expand_aliases_ = b; }
		bool autocd()      const { return autocd_; }      void setAutocd(bool b)      { autocd_ = b; }
		bool checkwinsize() const { return checkwinsize_; } void setCheckwinsize(bool b) { checkwinsize_ = b; }

		/// Set `$0` (the shell's own name).
		void   setShellName(std::string s) { shell_name_ = std::move(s); }
		/// Current value of `$0`.
		const std::string& shellName() const { return shell_name_; }

		/// Convenience: load all process env vars into vars_, exporting each.
		void   loadFromProcessEnv();

		// ---- Dynamic params: $RANDOM / $SECONDS / $LINENO ----
		/**
		 * @brief `$RANDOM` — advance the LCG and return the next value.
		 *
		 * Assigning to `RANDOM` (handled in set()) seeds the LCG
		 * without storing the value in `vars_`.
		 */
		unsigned int randomNext();
		/// Seed the `$RANDOM` LCG.
		void         setRandomSeed(unsigned int s) { random_state_ = s; }

		/**
		 * @brief `$SECONDS` — whole seconds since this Environment was
		 *        constructed, offset by any user assignment.
		 */
		long long    secondsSinceStart() const;
		/// Set the user-supplied offset for `$SECONDS`.
		void         setSecondsOffset(long long s);

		/// `$LINENO` — line of the currently executing command.
		int          currentLineno() const   { return current_lineno_; }
		/// Set `$LINENO`; called by the executor on each node entry.
		void         setCurrentLineno(int n) { current_lineno_ = n; }

	private:
		std::unordered_map<std::string, std::string> vars_;
		std::unordered_map<std::string, IndexedArray> indexed_;
		std::unordered_map<std::string, AssocArray> assoc_;
		std::unordered_set<std::string> exported_;
		std::unordered_set<std::string> readonly_;
		std::vector<std::string> positional_;
		int last_status_ = 0;
		long long last_bg_pid_ = 0;
		long long shell_pid_ = 0;
		std::string shell_name_ = "wbsh";
		bool errexit_ = false;
		bool nounset_ = false;
		bool xtrace_  = false;
		bool pipefail_ = false;
		bool noglob_  = false;
		// shopt flags. globstar is on by default since bash's `**` is
		// the more useful behaviour for an interactive shell.
		bool nullglob_     = false;
		bool dotglob_      = false;
		bool extglob_      = false;
		bool nocaseglob_   = false;
		bool nocasematch_  = false;
		bool globstar_     = true;
		bool lastpipe_     = false;
		bool huponexit_    = false;
		bool expand_aliases_ = true;
		bool autocd_       = false;
		bool checkwinsize_ = true;
		// Dynamic-param state.
		std::chrono::steady_clock::time_point start_time_ =
		    std::chrono::steady_clock::now();
		long long seconds_offset_ = 0;
		unsigned int random_state_ = 0;
		int current_lineno_ = 0;
	};

}  // namespace wbsh
