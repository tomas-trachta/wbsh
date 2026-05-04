#pragma once

#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wbsh {

	// Shell variable & parameter store. Holds named variables, the export set,
	// positional parameters ($1, $2, ...), and a few well-known special params
	// ($?, $$, $!, $-).
	class Environment {
	public:
		Environment();

		// ---- Named variables ----
		void   set(const std::string& name, std::string value);
		// Bulk write that bypasses the readonly check. Used by internal
		// state-restoration paths (subshell teardown, function-call locals)
		// where we're undoing changes the user couldn't have made directly.
		void   forceSet(const std::string& name, std::string value) {
			vars_[name] = std::move(value);
		}
		void   unset(const std::string& name);
		bool   has(const std::string& name) const;
		std::string get(const std::string& name) const;   // empty when unset

		void   exportVar(const std::string& name);
		void   unexportVar(const std::string& name);
		bool   isExported(const std::string& name) const;
		void   markReadonly(const std::string& name)   { readonly_.insert(name); }
		bool   isReadonly(const std::string& name) const { return readonly_.count(name) != 0; }
		const std::unordered_set<std::string>& readonlySet() const { return readonly_; }

		// Iterate variables for diagnostics / `set` output.
		const std::unordered_map<std::string, std::string>& vars() const { return vars_; }

		// ---- Indexed / associative arrays ----
		// A name lives in at most one of {vars_, indexed_, assoc_}. Bash
		// arrays are sparse, so an indexed array is a sorted map.
		using IndexedArray = std::map<long long, std::string>;
		using AssocArray   = std::map<std::string, std::string>;

		bool isIndexedArray(const std::string& name) const { return indexed_.count(name) != 0; }
		bool isAssocArray(const std::string& name) const   { return assoc_.count(name) != 0; }
		const IndexedArray* getIndexedArray(const std::string& name) const {
			auto it = indexed_.find(name);
			return it == indexed_.end() ? nullptr : &it->second;
		}
		const AssocArray* getAssocArray(const std::string& name) const {
			auto it = assoc_.find(name);
			return it == assoc_.end() ? nullptr : &it->second;
		}
		// Replace name with a fresh indexed array built from `values`
		// (assigned at indices 0..size-1). Removes any prior scalar/assoc.
		void setIndexedArrayFromList(const std::string& name,
		                             std::vector<std::string> values);
		// Replace name with a sparse indexed array (key/value pairs).
		// Removes any prior scalar/assoc.
		void setIndexedArraySparse(const std::string& name,
		                           std::map<long long, std::string> elems);
		// Set arr[idx]=val. If name was scalar, it becomes an indexed array
		// with the prior scalar at index 0 plus this assignment.
		void setIndexedElement(const std::string& name, long long idx, std::string val);
		// Declare an empty associative array (so future assoc[k]=v works).
		void declareAssocArray(const std::string& name);
		// Set assoc[key]=val. If name didn't exist, creates an assoc array.
		void setAssocElement(const std::string& name, std::string key, std::string val);
		// Drop any array form for `name`.
		void unsetArray(const std::string& name) {
			indexed_.erase(name);
			assoc_.erase(name);
		}
		// Iterate for `set` / `declare -p` output.
		const std::unordered_map<std::string, IndexedArray>& indexedArrays() const { return indexed_; }
		const std::unordered_map<std::string, AssocArray>&   assocArrays()   const { return assoc_;   }

		// ---- Positional parameters ----
		void   setPositional(std::vector<std::string> args);
		const std::vector<std::string>& positional() const { return positional_; }

		// ---- Special parameters ----
		void   setLastStatus(int s) { last_status_ = s; }
		int    lastStatus() const   { return last_status_; }

		void   setLastBgPid(long long p) { last_bg_pid_ = p; }
		long long lastBgPid() const      { return last_bg_pid_; }

		long long shellPid() const { return shell_pid_; }

		// Shell options ($-): rebuilt from individual flags.
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

		// `shopt` flags. Default values match bash's interactive defaults
		// where they match POSIX expectations.
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

		// $0, the shell name
		void   setShellName(std::string s) { shell_name_ = std::move(s); }
		const std::string& shellName() const { return shell_name_; }

		// Convenience: load all WBSH process env vars into vars_, exporting each.
		void   loadFromProcessEnv();

		// ---- Dynamic params: $RANDOM / $SECONDS / $LINENO ----
		// $RANDOM advances on each read. Assigning to RANDOM (handled in
		// `set`) seeds the LCG without storing the value in vars_.
		unsigned int randomNext();
		void         setRandomSeed(unsigned int s) { random_state_ = s; }

		// $SECONDS is whole seconds since the shell (this Environment) was
		// constructed, offset by any user assignment.
		long long    secondsSinceStart() const;
		void         setSecondsOffset(long long s);

		// $LINENO is the line of the currently-executing command. Updated
		// by the executor on each node entry.
		int          currentLineno() const   { return current_lineno_; }
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
