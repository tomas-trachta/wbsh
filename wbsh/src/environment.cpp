/**
 * @file environment.cpp
 * @brief Shell variable, array, and parameter store.
 */

#include "environment.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

#  include <process.h>
#else
#  include <unistd.h>
extern char** environ;
#endif /* _WIN32 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <utility>

#include "diagnostics.h"
#include "numparse.h"
#include "utf8.h"

namespace wbsh {

	static const char* const kDefaultIfs = " \t\n";

	// bash 5's generator, so `RANDOM=seed` gives bash's sequence: Park and
	// Miller's minimal standard x' = 16807 x mod (2^31 - 1), computed by
	// Schrage's method, then the two halves folded into 0..32767.
	static const long long    kParkMillerModulus    = 0x7fffffffLL;
	static const long long    kParkMillerMultiplier = 16807;
	static const long long    kParkMillerQuotient   = 127773;
	static const long long    kParkMillerRemainder  = 2836;
	static const unsigned int kZeroSeedReplacement  = 123459876u;
	static const unsigned int kRandomMask           = 32767u;

	static unsigned int parkMillerNext(unsigned int last) {
		const long long x = (last == 0) ? kZeroSeedReplacement : last;
		const long long high = x / kParkMillerQuotient;
		const long long low = x - kParkMillerQuotient * high;
		const long long t = kParkMillerMultiplier * low - kParkMillerRemainder * high;
		return static_cast<unsigned int>(t < 0 ? t + kParkMillerModulus : t);
	}

	Environment::Environment() {
#ifdef _WIN32
		shell_pid_ = static_cast<long long>(::GetCurrentProcessId());
#else
		shell_pid_ = static_cast<long long>(::getpid());
#endif
		bash_pid_ = shell_pid_;
		vars_["IFS"] = kDefaultIfs;
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		random_state_ = static_cast<unsigned int>(shell_pid_) ^ static_cast<unsigned int>(ticks);
	}

	const std::vector<SetOptionSpec>& setOptionTable() {
		static const std::vector<SetOptionSpec> table = {
			{ "allexport",            'a', &SetOptions::allexport },
			{ "braceexpand",          'B', &SetOptions::braceexpand },
			{ "emacs",                0,   &SetOptions::emacs },
			{ "errexit",              'e', &SetOptions::errexit },
			{ "errtrace",             'E', &SetOptions::errtrace },
			{ "functrace",            'T', &SetOptions::functrace },
			{ "hashall",              'h', &SetOptions::hashall },
			{ "histexpand",           'H', &SetOptions::histexpand },
			{ "history",              0,   &SetOptions::history },
			{ "ignoreeof",            0,   &SetOptions::ignoreeof },
			{ "interactive-comments", 0,   &SetOptions::interactive_comments },
			{ "keyword",              'k', &SetOptions::keyword },
			{ "monitor",              'm', &SetOptions::monitor },
			{ "noclobber",            'C', &SetOptions::noclobber },
			{ "noexec",               'n', &SetOptions::noexec },
			{ "noglob",               'f', &SetOptions::noglob },
			{ "nolog",                0,   &SetOptions::nolog },
			{ "notify",               'b', &SetOptions::notify },
			{ "nounset",              'u', &SetOptions::nounset },
			{ "onecmd",               't', &SetOptions::onecmd },
			{ "physical",             'P', &SetOptions::physical },
			{ "pipefail",             0,   &SetOptions::pipefail },
			{ "posix",                0,   &SetOptions::posix },
			{ "privileged",           'p', &SetOptions::privileged },
			{ "verbose",              'v', &SetOptions::verbose },
			{ "vi",                   0,   &SetOptions::vi },
			{ "xtrace",               'x', &SetOptions::xtrace },
		};

		return table;
	}

	const SetOptionSpec* findSetOption(const std::string& name) {
		for (const SetOptionSpec& spec : setOptionTable()) {
			if (name == spec.name) return &spec;
		}

		return nullptr;
	}

	const SetOptionSpec* findSetFlag(char letter) {
		if (letter == 0) return nullptr;

		for (const SetOptionSpec& spec : setOptionTable()) {
			if (spec.letter == letter) return &spec;
		}

		return nullptr;
	}

	static const ShoptSpec kShoptTable[] = {
		{ "array_expand_once",       &ShoptFlags::array_expand_once },
		{ "assoc_expand_once",       &ShoptFlags::array_expand_once },
		{ "autocd",                  &ShoptFlags::autocd },
		{ "bash_source_fullpath",    &ShoptFlags::bash_source_fullpath },
		{ "cdable_vars",             &ShoptFlags::cdable_vars },
		{ "cdspell",                 &ShoptFlags::cdspell },
		{ "checkhash",               &ShoptFlags::checkhash },
		{ "checkjobs",               &ShoptFlags::checkjobs },
		{ "checkwinsize",            &ShoptFlags::checkwinsize },
		{ "cmdhist",                 &ShoptFlags::cmdhist },
		{ "compat31",                &ShoptFlags::compat31 },
		{ "compat32",                &ShoptFlags::compat32 },
		{ "compat40",                &ShoptFlags::compat40 },
		{ "compat41",                &ShoptFlags::compat41 },
		{ "compat42",                &ShoptFlags::compat42 },
		{ "compat43",                &ShoptFlags::compat43 },
		{ "compat44",                &ShoptFlags::compat44 },
		{ "complete_fullquote",      &ShoptFlags::complete_fullquote },
		{ "direxpand",               &ShoptFlags::direxpand },
		{ "dirspell",                &ShoptFlags::dirspell },
		{ "dotglob",                 &ShoptFlags::dotglob },
		{ "execfail",                &ShoptFlags::execfail },
		{ "expand_aliases",          &ShoptFlags::expand_aliases },
		{ "extdebug",                &ShoptFlags::extdebug },
		{ "extglob",                 &ShoptFlags::extglob },
		{ "extquote",                &ShoptFlags::extquote },
		{ "failglob",                &ShoptFlags::failglob },
		{ "force_fignore",           &ShoptFlags::force_fignore },
		{ "globasciiranges",         &ShoptFlags::globasciiranges },
		{ "globskipdots",            &ShoptFlags::globskipdots },
		{ "globstar",                &ShoptFlags::globstar },
		{ "gnu_errfmt",              &ShoptFlags::gnu_errfmt },
		{ "histappend",              &ShoptFlags::histappend },
		{ "histreedit",              &ShoptFlags::histreedit },
		{ "histverify",              &ShoptFlags::histverify },
		{ "hostcomplete",            &ShoptFlags::hostcomplete },
		{ "huponexit",               &ShoptFlags::huponexit },
		{ "inherit_errexit",         &ShoptFlags::inherit_errexit },
		{ "interactive_comments",    &ShoptFlags::interactive_comments },
		{ "lastpipe",                &ShoptFlags::lastpipe },
		{ "lithist",                 &ShoptFlags::lithist },
		{ "localvar_inherit",        &ShoptFlags::localvar_inherit },
		{ "localvar_unset",          &ShoptFlags::localvar_unset },
		{ "login_shell",             &ShoptFlags::login_shell },
		{ "mailwarn",                &ShoptFlags::mailwarn },
		{ "no_empty_cmd_completion", &ShoptFlags::no_empty_cmd_completion },
		{ "nocaseglob",              &ShoptFlags::nocaseglob },
		{ "nocasematch",             &ShoptFlags::nocasematch },
		{ "noexpand_translation",    &ShoptFlags::noexpand_translation },
		{ "nullglob",                &ShoptFlags::nullglob },
		{ "patsub_replacement",      &ShoptFlags::patsub_replacement },
		{ "progcomp",                &ShoptFlags::progcomp },
		{ "progcomp_alias",          &ShoptFlags::progcomp_alias },
		{ "promptvars",              &ShoptFlags::promptvars },
		{ "restricted_shell",        &ShoptFlags::restricted_shell },
		{ "shift_verbose",           &ShoptFlags::shift_verbose },
		{ "sourcepath",              &ShoptFlags::sourcepath },
		{ "varredir_close",          &ShoptFlags::varredir_close },
		{ "xpg_echo",                &ShoptFlags::xpg_echo },
	};

	const std::vector<ShoptSpec>& shoptTable() {
		static const std::vector<ShoptSpec> table(std::begin(kShoptTable), std::end(kShoptTable));
		return table;
	}

	const ShoptSpec* findShopt(const std::string& name) {
		for (const ShoptSpec& spec : shoptTable()) {
			if (name == spec.name) return &spec;
		}

		return nullptr;
	}

	std::string Environment::shellOptions() const {
		static const char* const kDollarDashOrder = "abefhikmnprtuvxBCEHPT";

		std::string letters;
		for (const char* c = kDollarDashOrder; *c != '\0'; ++c) {
			if (*c == 'i') {
				if (interactive_) letters += 'i';
				continue;
			}

			if (*c == 'r') {
				if (shopt_.restricted_shell) letters += 'r';
				continue;
			}

			const SetOptionSpec* spec = findSetFlag(*c);
			if (spec != nullptr && options_.*(spec->flag)) letters += *c;
		}

		if (invocation_letter_ != 0) letters += invocation_letter_;
		return letters;
	}

	static void appendColonSeparated(std::string& list, const char* name) {
		if (!list.empty()) list.push_back(':');
		list += name;
	}

	std::string Environment::enabledSetOptionNames() const {
		std::string names;
		for (const SetOptionSpec& spec : setOptionTable()) {
			if (options_.*(spec.flag)) appendColonSeparated(names, spec.name);
		}

		return names;
	}

	std::string Environment::enabledShoptNames() const {
		std::string names;
		for (const ShoptSpec& spec : shoptTable()) {
			if (shopt_.*(spec.flag)) appendColonSeparated(names, spec.name);
		}

		return names;
	}

	void Environment::setInteractive(bool b) {
		interactive_ = b;
		options_.emacs = b;
		options_.history = b;
		options_.histexpand = b;
		shopt_.expand_aliases = b;
	}

	static bool namesUtf8(std::string locale) {
		for (char& c : locale) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return locale.find("utf-8") != std::string::npos || locale.find("utf8") != std::string::npos;
	}

	bool Environment::utf8Locale() const {
		for (const char* name : { "LC_ALL", "LC_CTYPE", "LANG" }) {
			const std::string value = get(name);
			if (!value.empty()) return namesUtf8(value);
		}

		return true;
	}

	std::string Environment::ctypeLocale() const {
		for (const char* name : { "LC_ALL", "LC_CTYPE", "LANG" }) {
			const std::string value = get(name);
			if (!value.empty()) return value;
		}

		return std::string();
	}

	std::string Environment::numericLocale() const {
		for (const char* name : { "LC_ALL", "LC_NUMERIC", "LANG" }) {
			const std::string value = get(name);
			if (!value.empty()) return value;
		}

		return std::string();
	}

	bool Environment::rejectIfReadonly(const std::string& name) const {
		if (readonly_.count(name) == 0) return false;

		shellErrorf("%s: readonly variable\n", name.c_str());
		return true;
	}

	// Like bash, a chain longer than this is treated as circular.
	static const int kMaxNamerefDepth = 8;

	const std::string& Environment::resolveRef(const std::string& name) const {
		if (namerefs_.empty()) return name;

		const std::string* current = &name;
		for (int depth = 0; depth < kMaxNamerefDepth; ++depth) {
			const auto found = namerefs_.find(*current);
			if (found == namerefs_.end() || found->second.empty()) return *current;

			current = &found->second;
		}

		return *current;
	}

	std::string Environment::namerefTarget(const std::string& name) const {
		const auto found = namerefs_.find(name);
		return found == namerefs_.end() ? std::string() : found->second;
	}

	void Environment::setNameref(const std::string& name, std::string target) {
		vars_.erase(name);
		indexed_.erase(name);
		assoc_.erase(name);
		namerefs_[name] = std::move(target);
	}

	void Environment::clearNameref(const std::string& name) {
		const auto found = namerefs_.find(name);
		if (found == namerefs_.end()) return;

		std::string target = std::move(found->second);
		namerefs_.erase(found);
		if (!target.empty()) vars_[name] = std::move(target);
	}

	void Environment::unsetNameref(const std::string& name) {
		if (namerefs_.erase(name) == 0) {
			unset(name);
			return;
		}

		exported_.erase(name);
		attributes_.erase(name);
	}

	// `a[1]` as a nameref target: the subscript of an indexed array is
	// arithmetic, that of an associative array a key.
	bool Environment::splitElementRef(const std::string& ref, ElementRef& out) const {
		if (ref.empty() || ref.back() != ']') return false;

		const std::size_t open = ref.find('[');
		if (open == std::string::npos || open == 0) return false;

		out.base = ref.substr(0, open);
		out.key = ref.substr(open + 1, ref.size() - open - 2);
		out.assoc = assoc_.count(out.base) != 0;
		out.whole = out.key == "@" || out.key == "*";
		if (out.assoc || out.whole) return true;

		if (subscript_evaluator_ == nullptr) {
			if (!parseLL(out.key, out.index)) out.index = 0;
			return true;
		}

		out.bad_index = !subscript_evaluator_(arith_context_, out.key, out.index);
		return true;
	}

	std::string Environment::wholeArrayText(const ElementRef& element) const {
		std::string text;
		const auto append = [&text](const std::string& value) {
			if (!text.empty()) text.push_back(' ');
			text += value;
		};

		if (element.assoc) {
			for (const auto& entry : assoc_.at(element.base)) append(entry.second);
		} else if (const auto indexed = indexed_.find(element.base); indexed != indexed_.end()) {
			for (const auto& entry : indexed->second) append(entry.second);
		} else {
			text = getResolved(element.base);
		}

		return text;
	}

	std::string Environment::getElement(const ElementRef& element) const {
		if (element.whole) return wholeArrayText(element);
		if (element.assoc) {
			const AssocArray& array = assoc_.at(element.base);
			const auto found = array.find(element.key);
			return found == array.end() ? std::string() : found->second;
		}

		const auto indexed = indexed_.find(element.base);
		if (indexed == indexed_.end()) return element.index == 0 ? getResolved(element.base) : "";

		const auto found = indexed->second.find(element.index);
		return found == indexed->second.end() ? std::string() : found->second;
	}

	bool Environment::hasElement(const ElementRef& element) const {
		if (element.whole) return !wholeArrayText(element).empty() || has(element.base);
		if (element.assoc) return assoc_.at(element.base).count(element.key) != 0;

		const auto indexed = indexed_.find(element.base);
		if (indexed == indexed_.end()) return element.index == 0 && hasResolved(element.base);
		return indexed->second.count(element.index) != 0;
	}

	static bool isReadOnlyDynamicName(const std::string& name) {
		return name == "LINENO" || name == "BASHPID" || name == "BASH_COMMAND"
			|| name == "EPOCHSECONDS" || name == "EPOCHREALTIME" || name == "BASH_MONOSECONDS"
			|| name == "BASHOPTS" || name == "SHELLOPTS";
	}

	// bash's noassign variables: the shell maintains them, scripts' writes vanish.
	static bool isNoAssignName(const std::string& name) {
		return name == "FUNCNAME" || name == "GROUPS";
	}

	static bool isRefName(const std::string& name) {
		if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0]))) return false;

		return std::all_of(name.begin(), name.end(), [](char c) {
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		});
	}

	// A name, or an element `name[subscript]`.
	static bool isValidRefTarget(const std::string& target) {
		const std::size_t bracket = target.find('[');
		if (!isRefName(target.substr(0, bracket))) return false;

		return bracket == std::string::npos || (target.size() > bracket + 2 && target.back() == ']');
	}

	bool Environment::rejectsRefTarget(const std::string& name, const std::string& value) const {
		const auto unbound = namerefs_.find(name);
		if (unbound == namerefs_.end() || !unbound->second.empty()) return false;

		return rejectsRefValue(value);
	}

	bool Environment::rejectsRefValue(const std::string& value) const {
		if (isValidRefTarget(value)) return false;

		const std::string prefix = assigning_command_.empty() ? "" : assigning_command_ + ": ";
		shellErrorf("%s`%s': not a valid identifier\n", prefix.c_str(), value.c_str());
		return true;
	}

	// An array made under the name of a nameref without a target replaces it.
	void Environment::dropUnboundNameref(const std::string& name) {
		const auto found = namerefs_.find(name);
		if (found == namerefs_.end() || !found->second.empty()) return;

		shellErrorf("warning: %s: removing nameref attribute\n", name.c_str());
		unsetNameref(name);
	}

	void Environment::set(const std::string& name, std::string value) {
		const auto unbound = namerefs_.find(name);
		if (unbound != namerefs_.end() && unbound->second.empty()) {
			if (!rejectsRefTarget(name, value)) unbound->second = std::move(value);
			return;
		}

		const std::string& target = resolveRef(name);
		ElementRef element;
		if (!splitElementRef(target, element)) {
			setResolved(target, std::move(value));
			return;
		}

		if (element.bad_index) return;
		if (element.whole && !element.assoc) {
			shellErrorf("%s: bad array subscript\n", target.c_str());
			return;
		}

		if (element.assoc) setAssocElement(element.base, element.key, std::move(value));
		else setIndexedElement(element.base, element.index, std::move(value));
	}

	void Environment::setResolved(const std::string& name, std::string value) {
		if (name == "RANDOM") {
			unsigned long seed = 0;
			if (parseUL(value, seed)) setRandomSeed(static_cast<unsigned int>(seed));
			return;
		}

		if (name == "SECONDS") {
			long long seconds = 0;
			if (parseLL(value, seconds)) setSecondsOffset(seconds);
			return;
		}

		if (name == "BASH_ARGV0") {
			shell_name_ = std::move(value);
			return;
		}

		if (rejectIfReadonly(name)) return;

		// Read-only dynamic params; writes are ignored silently.
		if (isReadOnlyDynamicName(name) || isNoAssignName(name)) return;

		if (!applyAttributes(name, value)) return;

		valueless_.erase(name);
		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) {
			indexed->second[0] = std::move(value);
			return;
		}

		// As in bash, a value for an associative array's name is its key "0".
		auto assoc = assoc_.find(name);
		if (assoc != assoc_.end()) {
			assoc->second["0"] = std::move(value);
			return;
		}

		vars_[name] = std::move(value);
		if (options_.allexport) exported_.insert(name);
		if (name == "GLOBIGNORE") noteGlobIgnore();
		if (name == "POSIXLY_CORRECT") notePosixlyCorrect();
		if (name == "OPTIND") ++optind_writes_;
		if (name == "IGNOREEOF") options_.ignoreeof = true;
		if (name == "PWD") indexed_["DIRSTACK"][0] = vars_[name];
	}

	// `set -r`: for good; the variables that would lead out of the
	// restriction become readonly, as in bash.
	void Environment::enterRestrictedMode() {
		static const char* const kRestrictedVariables[] = {
			"PATH", "SHELL", "ENV", "BASH_ENV", "HISTFILE",
		};

		shopt_.restricted_shell = true;
		for (const char* name : kRestrictedVariables) readonly_.insert(name);
	}

	void Environment::setPosixMode(bool on) {
		if (on == options_.posix) return;

		options_.posix = on;
		if (on) {
			posix_saved_ = { shopt_.expand_aliases, shopt_.inherit_errexit, shopt_.shift_verbose,
				options_.interactive_comments };
			shopt_.expand_aliases = shopt_.inherit_errexit = shopt_.shift_verbose = true;
			options_.interactive_comments = true;
			if (vars_.count("POSIXLY_CORRECT") == 0) set("POSIXLY_CORRECT", "y");
			return;
		}

		shopt_.expand_aliases = posix_saved_.expand_aliases;
		shopt_.inherit_errexit = posix_saved_.inherit_errexit;
		shopt_.shift_verbose = posix_saved_.shift_verbose;
		options_.interactive_comments = posix_saved_.interactive_comments;
		unset("POSIXLY_CORRECT");
	}

	// As in bash, setting POSIXLY_CORRECT turns POSIX mode on and unsetting
	// it turns it off.
	void Environment::notePosixlyCorrect() {
		setPosixMode(vars_.count("POSIXLY_CORRECT") != 0);
	}

	static bool hasGlobIgnorePattern(const std::string& value) {
		return value.find_first_not_of(':') != std::string::npos;
	}

	// As in bash, a GLOBIGNORE with a pattern turns dotglob on and unsetting
	// it turns dotglob off; an empty value leaves dotglob as it is.
	void Environment::noteGlobIgnore() {
		const auto value = vars_.find("GLOBIGNORE");
		if (value == vars_.end()) shopt_.dotglob = false;
		else if (hasGlobIgnorePattern(value->second)) shopt_.dotglob = true;
	}

	void Environment::setIndexedArrayFromList(const std::string& name,
			std::vector<std::string> values) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		vars_.erase(target);
		assoc_.erase(target);
		valueless_.erase(target);

		IndexedArray array;
		for (std::size_t i = 0; i < values.size(); ++i) {
			if (!applyAttributes(target, values[i])) continue;
			array[static_cast<long long>(i)] = std::move(values[i]);
		}

		indexed_[target] = std::move(array);
	}

	void Environment::setIndexedArraySparse(const std::string& name,
			std::map<long long, std::string> elems) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		for (auto& element : elems) applyAttributes(target, element.second);
		valueless_.erase(target);
		vars_.erase(target);
		assoc_.erase(target);
		indexed_[target] = std::move(elems);
	}

	void Environment::setIndexedElement(const std::string& name, long long idx, std::string val) {
		const std::string& target = resolveRef(name);
		if (isNoAssignName(target) || rejectIfReadonly(target)) return;
		if (!applyAttributes(target, val)) return;

		if (assoc_.count(target) != 0) {
			assoc_[target][std::to_string(idx)] = std::move(val);
			return;
		}

		if (!resolveElementIndex(target, idx)) return;

		valueless_.erase(target);
		auto indexed = indexed_.find(target);
		if (indexed != indexed_.end()) {
			indexed->second[idx] = std::move(val);
			return;
		}

		IndexedArray array;
		auto scalar = vars_.find(target);
		if (scalar != vars_.end()) {
			array[0] = std::move(scalar->second);
			vars_.erase(scalar);
		}

		array[idx] = std::move(val);
		indexed_[target] = std::move(array);
	}

	// A variable that already existed is converted, which in bash gives it
	// a smaller hash table and so another key order.
	void Environment::declareAssocArray(const std::string& name, bool converted) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		vars_.erase(target);
		indexed_.erase(target);
		valueless_.erase(target);
		const auto made = assoc_.emplace(target, AssocArray{});
		if (made.second && converted) made.first->second.useDefaultHashSize();
	}

	void Environment::sizeAssocTable(const std::string& name, std::uint32_t buckets) {
		const auto array = assoc_.find(resolveRef(name));
		if (array != assoc_.end() && array->second.empty()) array->second.useBuckets(buckets);
	}

	void Environment::resetAssocArray(const std::string& name) {
		declareAssocArray(name);

		const auto array = assoc_.find(resolveRef(name));
		if (array != assoc_.end() && !isReadonly(name)) array->second.clear();
	}

	void Environment::setAssocElement(const std::string& name, std::string key, std::string val) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;
		if (!applyAttributes(target, val)) return;

		valueless_.erase(target);
		auto assoc = assoc_.find(target);
		if (assoc != assoc_.end()) {
			assoc->second[std::move(key)] = std::move(val);
			return;
		}

		vars_.erase(target);
		indexed_.erase(target);
		assoc_[target][std::move(key)] = std::move(val);
	}

	void Environment::unsetElement(const std::string& name, long long idx, const std::string& key) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		auto assoc = assoc_.find(target);
		if (assoc != assoc_.end()) {
			assoc->second.erase(key);
			return;
		}

		auto indexed = indexed_.find(target);
		if (indexed == indexed_.end() || !resolveElementIndex(target, idx)) return;
		indexed->second.erase(idx);
	}

	bool Environment::resolveElementIndex(const std::string& name, long long& idx) const {
		if (idx >= 0) return true;

		const std::string& target = resolveRef(name);
		const auto indexed = indexed_.find(target);
		long long end = vars_.count(target) != 0 ? 1 : 0;
		if (indexed != indexed_.end() && !indexed->second.empty()) {
			end = indexed->second.rbegin()->first + 1;
		}

		if (idx + end >= 0) {
			idx += end;
			return true;
		}

		shellErrorf("%s[%lld]: bad array subscript\n", target.c_str(), idx);
		return false;
	}

	// Unsetting a nameref unsets the variable it names; one with no
	// target goes itself.
	// False only when an element's subscript failed to evaluate.
	bool Environment::unset(const std::string& name) {
		const std::string target = resolveRef(name);
		if (target == name) namerefs_.erase(name);

		ElementRef element;
		if (splitElementRef(target, element)) {
			if (element.bad_index) return false;
			if (element.whole && !element.assoc) {
				const auto indexed = indexed_.find(element.base);
				if (indexed != indexed_.end()) indexed->second.clear();
				return true;
			}

			unsetElement(element.base, element.index, element.key);
			return true;
		}

		vars_.erase(target);
		indexed_.erase(target);
		assoc_.erase(target);
		exported_.erase(target);
		attributes_.erase(target);
		valueless_.erase(target);
		if (target == "GLOBIGNORE") noteGlobIgnore();
		if (target == "POSIXLY_CORRECT") notePosixlyCorrect();
		if (target == "IGNOREEOF") options_.ignoreeof = false;
		return true;
	}

	bool Environment::has(const std::string& name) const {
		const std::string& target = resolveRef(name);
		ElementRef element;
		if (splitElementRef(target, element)) return !element.bad_index && hasElement(element);
		return hasResolved(target);
	}

	bool Environment::hasResolved(const std::string& name) const {
		if (vars_.find(name) != vars_.end()) return true;
		if (indexed_.count(name) != 0) return true;
		if (assoc_.count(name) != 0) return true;
		return false;
	}

	std::string Environment::ifs() const {
		return has("IFS") ? get("IFS") : std::string(kDefaultIfs);
	}

	std::string Environment::get(const std::string& name) const {
		const std::string& target = resolveRef(name);
		ElementRef element;
		if (!splitElementRef(target, element)) return getResolved(target);

		return element.bad_index ? std::string() : getElement(element);
	}

	std::string Environment::getResolved(const std::string& name) const {
		auto scalar = vars_.find(name);
		if (scalar != vars_.end()) return scalar->second;

		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) {
			auto element = indexed->second.find(0);
			return element == indexed->second.end() ? std::string() : element->second;
		}

		auto assoc = assoc_.find(name);
		if (assoc != assoc_.end()) {
			auto element = assoc->second.find("0");
			return element == assoc->second.end() ? std::string() : element->second;
		}

		return {};
	}

	void Environment::exportVar(const std::string& name) {
		exported_.insert(resolveRef(name));
	}

	void Environment::unexportVar(const std::string& name) {
		exported_.erase(resolveRef(name));
	}

	bool Environment::isExported(const std::string& name) const {
		return exported_.count(resolveRef(name)) != 0;
	}

	bool Environment::exportedEntry(const std::string& name, std::string& out) const {
		const auto found = vars_.find(name);
		if (found == vars_.end() || exported_.count(name) == 0) return false;

		out = found->second;
		return true;
	}

	unsigned Environment::attributes(const std::string& name) const {
		const auto found = attributes_.find(resolveRef(name));
		return found == attributes_.end() ? 0u : found->second;
	}

	std::string Environment::attributeLetters(const std::string& name) const {
		const unsigned attrs = attributes(name);
		std::string letters;
		if (isIndexedArray(name))          letters += 'a';
		if (isAssocArray(name))            letters += 'A';
		if ((attrs & kAttrInteger) != 0)   letters += 'i';
		if (isReadonly(name))              letters += 'r';
		if (isExported(name))              letters += 'x';
		if ((attrs & kAttrLowercase) != 0) letters += 'l';
		if ((attrs & kAttrUppercase) != 0) letters += 'u';
		if ((attrs & kAttrCapitalize) != 0) letters += 'c';
		return letters;
	}

	void Environment::setAttributes(const std::string& name, unsigned attributes) {
		const std::string& target = resolveRef(name);
		if (attributes == 0) {
			attributes_.erase(target);
			return;
		}

		attributes_[target] = attributes;
	}

	std::string Environment::appendedValue(const std::string& name,
			const std::string& suffix) const {
		return appendedTo(name, get(name), suffix);
	}

	std::string Environment::appendedTo(const std::string& name, const std::string& current,
			const std::string& suffix) const {
		if ((attributes(name) & kAttrInteger) == 0) return current + suffix;
		if (current.empty()) return suffix;

		return "(" + current + ")+(" + suffix + ")";
	}

	static void transformCase(std::string& value, bool upper) {
		std::string converted;
		converted.reserve(value.size());
		for (std::size_t pos = 0; pos < value.size();) {
			const Utf8Char c = decodeUtf8(value, pos);
			appendUtf8(converted, upper ? upperCode(c.code) : lowerCode(c.code));
			pos += c.length;
		}

		value = std::move(converted);
	}

	// `declare -c`: the first character upper case, the rest lower case.
	static void capitalize(std::string& value) {
		if (value.empty()) return;

		const Utf8Char first = decodeUtf8(value, 0);
		std::string rest = value.substr(first.length);
		transformCase(rest, false);

		std::string converted;
		appendUtf8(converted, upperCode(first.code));
		value = converted + rest;
	}

	bool Environment::applyAttributes(const std::string& name, std::string& value) const {
		const unsigned attrs = attributes(name);
		if (attrs == 0) return true;

		if ((attrs & kAttrInteger) != 0 && arith_evaluator_ != nullptr) {
			long long number = 0;
			if (!arith_evaluator_(arith_context_, value, number)) return false;
			value = std::to_string(number);
		}

		if ((attrs & kAttrLowercase) != 0) transformCase(value, false);
		if ((attrs & kAttrUppercase) != 0) transformCase(value, true);
		if ((attrs & kAttrCapitalize) != 0) capitalize(value);
		return true;
	}

	template <typename Map>
	static const typename Map::mapped_type* findEntry(const Map& map, const std::string& name) {
		const auto found = map.find(name);
		return found == map.end() ? nullptr : &found->second;
	}

	// Saves @p name itself, without following a nameref.
	Environment::VarState Environment::saveVar(const std::string& name) const {
		VarState state;
		if (const std::string* scalar = findEntry(vars_, name)) {
			state.has_scalar = true;
			state.scalar = *scalar;
		}

		if (const IndexedArray* indexed = findEntry(indexed_, name)) {
			state.has_indexed = true;
			state.indexed = *indexed;
		}

		if (const AssocArray* assoc = findEntry(assoc_, name)) {
			state.has_assoc = true;
			state.assoc = *assoc;
		}

		if (const std::string* target = findEntry(namerefs_, name)) {
			state.has_nameref = true;
			state.nameref = *target;
		}

		const unsigned* attributes = findEntry(attributes_, name);
		state.exported = exported_.count(name) != 0;
		state.readonly = readonly_.count(name) != 0;
		state.valueless = valueless_.count(name) != 0;
		state.attributes = attributes == nullptr ? 0u : *attributes;
		return state;
	}

	template <typename Set>
	static void setMembership(Set& set, const std::string& name, bool member) {
		if (member) set.insert(name);
		else        set.erase(name);
	}

	void Environment::restoreVar(const std::string& name, VarState state) {
		vars_.erase(name);
		indexed_.erase(name);
		assoc_.erase(name);
		namerefs_.erase(name);
		if (state.has_scalar)  vars_[name] = std::move(state.scalar);
		if (state.has_indexed) indexed_[name] = std::move(state.indexed);
		if (state.has_assoc)   assoc_[name] = std::move(state.assoc);
		if (state.has_nameref) namerefs_[name] = std::move(state.nameref);

		setMembership(exported_, name, state.exported);
		setMembership(readonly_, name, state.readonly);
		setMembership(valueless_, name, state.valueless);
		if (state.attributes == 0) attributes_.erase(name);
		else attributes_[name] = state.attributes;
		if (name == "POSIXLY_CORRECT") notePosixlyCorrect();
	}

	void Environment::setPositional(std::vector<std::string> args) {
		positional_ = std::move(args);
	}

	unsigned int Environment::randomNext() {
		random_state_ = parkMillerNext(random_state_);
		return ((random_state_ >> 16) ^ (random_state_ & 0xffffu)) & kRandomMask;
	}

	void Environment::reseedRandom() {
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		random_state_ = randomNext() ^ static_cast<unsigned int>(ticks);
	}

	void Environment::enterSubshellPid() {
		static long long serial = 0;
#ifdef _WIN32
		const long long pid = static_cast<long long>(::GetCurrentProcessId());
#else
		const long long pid = static_cast<long long>(::getpid());
#endif
		bash_pid_ = ((++serial) << 32) | pid;
		++subshell_level_;
	}

	template <typename Map>
	static void dropEntriesNotIn(Map& current, const Map& snapshot) {
		for (auto it = current.begin(); it != current.end(); ) {
			if (snapshot.count(it->first) == 0) it = current.erase(it);
			else ++it;
		}
	}

	template <typename Map>
	static void restoreEntries(Map& current, const Map& snapshot) {
		dropEntriesNotIn(current, snapshot);

		for (const auto& entry : snapshot) {
			auto found = current.find(entry.first);
			if (found == current.end()) current.emplace(entry.first, entry.second);
			else if (found->second != entry.second) found->second = entry.second;
		}
	}

	void Environment::restoreFrom(const Environment& snapshot) {
		restoreEntries(vars_, snapshot.vars_);
		restoreEntries(indexed_, snapshot.indexed_);
		restoreEntries(assoc_, snapshot.assoc_);

		exported_     = snapshot.exported_;
		readonly_     = snapshot.readonly_;
		attributes_   = snapshot.attributes_;
		namerefs_     = snapshot.namerefs_;
		positional_   = snapshot.positional_;
		last_bg_pid_  = snapshot.last_bg_pid_;
		bash_pid_     = snapshot.bash_pid_;
		subshell_level_ = snapshot.subshell_level_;
		shell_name_   = snapshot.shell_name_;
		options_      = snapshot.options_;
		interactive_  = snapshot.interactive_;
		shopt_        = snapshot.shopt_;

		seconds_offset_ = snapshot.seconds_offset_;
		random_state_   = snapshot.random_state_;
		current_lineno_ = snapshot.current_lineno_;
	}

	long long Environment::elapsedSeconds() const {
		const auto now = std::chrono::steady_clock::now();
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_);
		return static_cast<long long>(seconds.count());
	}

	long long Environment::secondsSinceStart() const {
		return elapsedSeconds() - seconds_offset_;
	}

	void Environment::setSecondsOffset(long long s) {
		seconds_offset_ = elapsedSeconds() - s;
	}

	void Environment::importProcessVariable(std::string name, std::string value) {
		const bool posixly_correct = name == "POSIXLY_CORRECT";
		vars_[name] = std::move(value);
		exported_.insert(std::move(name));
		if (posixly_correct) notePosixlyCorrect();
	}

#ifdef _WIN32
	static std::string wideToUtf8(const std::wstring& wide) {
		if (wide.empty()) return {};

		const int size = static_cast<int>(wide.size());
		const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size,
			nullptr, 0, nullptr, nullptr);
		std::string utf8(length, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, wide.data(), size, utf8.data(), length, nullptr, nullptr);
		return utf8;
	}

	// Windows variable names are case-insensitive and the OS spells the
	// search path `Path`. The shell reads and rewrites it as `PATH`, so the
	// imported name must be folded or the child block carries both
	// spellings and each child picks one of them at random.
	static std::string canonicalProcessVariableName(std::string name) {
		if (name.size() != 4) return name;

		std::string lower = name;
		for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return lower == "path" ? std::string("PATH") : name;
	}

	void Environment::loadFromProcessEnv() {
		LPWCH block = ::GetEnvironmentStringsW();
		if (block == nullptr) return;

		for (LPWCH cursor = block; *cursor != L'\0'; ) {
			const std::wstring entry = cursor;
			cursor += entry.size() + 1;

			const std::size_t eq = entry.find(L'=');
			if (eq == std::wstring::npos || eq == 0) continue;

			importProcessVariable(canonicalProcessVariableName(wideToUtf8(entry.substr(0, eq))),
				wideToUtf8(entry.substr(eq + 1)));
		}

		::FreeEnvironmentStringsW(block);
	}
#else
	void Environment::loadFromProcessEnv() {
		for (char** cursor = environ; *cursor != nullptr; ++cursor) {
			const std::string entry = *cursor;
			const std::size_t eq = entry.find('=');
			if (eq == std::string::npos || eq == 0) continue;

			importProcessVariable(entry.substr(0, eq), entry.substr(eq + 1));
		}
	}
#endif /* _WIN32 */

	static std::size_t closingQuote(const std::string& text, std::size_t open) {
		const std::size_t close = text.find(text[open], open + 1);
		return close == std::string::npos ? text.size() : close;
	}

	bool isAssocKeyReference(const Environment& env, const std::string& text) {
		const std::size_t bracket = text.find('[');
		if (!env.shopts().array_expand_once || bracket == std::string::npos) return false;
		if (text.size() < bracket + 3 || text.back() != ']') return false;

		return env.isAssocArray(text.substr(0, bracket));
	}

	std::size_t subscriptEnd(const std::string& text, std::size_t open, bool skips_quotes) {
		int depth = 0;
		for (std::size_t i = open; i < text.size(); ++i) {
			const char c = text[i];
			if (skips_quotes && (c == '\'' || c == '"')) i = closingQuote(text, i);
			else if (c == '[') ++depth;
			else if (c == ']' && --depth == 0) return i;
		}

		return std::string::npos;
	}

}  // namespace wbsh
