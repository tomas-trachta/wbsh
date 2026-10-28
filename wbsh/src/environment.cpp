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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "numparse.h"
#include "utf8.h"

namespace wbsh {

	static const char* const kDefaultIfs = " \t\n";

	// Classic LCG; the high half masked to 0..32767 to match bash.
	static const unsigned int kRandomMultiplier = 1103515245u;
	static const unsigned int kRandomIncrement  = 12345u;
	static const unsigned int kRandomDivisor    = 65536u;
	static const unsigned int kRandomRange      = 32768u;

	Environment::Environment() {
#ifdef _WIN32
		shell_pid_ = static_cast<long long>(::GetCurrentProcessId());
#else
		shell_pid_ = static_cast<long long>(::getpid());
#endif
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

	const std::vector<ShoptSpec>& shoptTable() {
		static const std::vector<ShoptSpec> table = {
			{ "nullglob",       &ShoptFlags::nullglob },
			{ "dotglob",        &ShoptFlags::dotglob },
			{ "extglob",        &ShoptFlags::extglob },
			{ "nocaseglob",     &ShoptFlags::nocaseglob },
			{ "nocasematch",    &ShoptFlags::nocasematch },
			{ "globstar",       &ShoptFlags::globstar },
			{ "lastpipe",       &ShoptFlags::lastpipe },
			{ "huponexit",      &ShoptFlags::huponexit },
			{ "expand_aliases", &ShoptFlags::expand_aliases },
			{ "autocd",         &ShoptFlags::autocd },
			{ "checkwinsize",   &ShoptFlags::checkwinsize },
			{ "promptvars",     &ShoptFlags::promptvars },
		};

		return table;
	}

	const ShoptSpec* findShopt(const std::string& name) {
		for (const ShoptSpec& spec : shoptTable()) {
			if (name == spec.name) return &spec;
		}

		return nullptr;
	}

	std::string Environment::shellOptions() const {
		static const char* const kDollarDashOrder = "abefhikmnptuvxBCEHPT";

		std::string letters;
		for (const char* c = kDollarDashOrder; *c != '\0'; ++c) {
			if (*c == 'i') {
				if (interactive_) letters += 'i';
				continue;
			}

			const SetOptionSpec* spec = findSetFlag(*c);
			if (spec != nullptr && options_.*(spec->flag)) letters += *c;
		}

		return letters;
	}

	void Environment::setInteractive(bool b) {
		interactive_ = b;
		options_.emacs = b;
		options_.history = b;
		options_.histexpand = b;
	}

	bool Environment::rejectIfReadonly(const std::string& name) const {
		if (readonly_.count(name) == 0) return false;

		std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
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
		if (out.assoc) return true;

		if (arith_evaluator_ == nullptr || !arith_evaluator_(arith_context_, out.key, out.index)) {
			if (!parseLL(out.key, out.index)) out.index = 0;
		}

		return true;
	}

	std::string Environment::getElement(const ElementRef& element) const {
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
		if (element.assoc) return assoc_.at(element.base).count(element.key) != 0;

		const auto indexed = indexed_.find(element.base);
		if (indexed == indexed_.end()) return element.index == 0 && hasResolved(element.base);
		return indexed->second.count(element.index) != 0;
	}

	void Environment::set(const std::string& name, std::string value) {
		const auto unbound = namerefs_.find(name);
		if (unbound != namerefs_.end() && unbound->second.empty()) {
			unbound->second = std::move(value);
			return;
		}

		const std::string& target = resolveRef(name);
		ElementRef element;
		if (!splitElementRef(target, element)) {
			setResolved(target, std::move(value));
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

		// Read-only dynamic params; writes are ignored silently.
		if (name == "LINENO" || name == "BASHPID") return;

		if (rejectIfReadonly(name)) return;
		if (!applyAttributes(name, value)) return;

		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) {
			indexed->second[0] = std::move(value);
			return;
		}

		assoc_.erase(name);
		vars_[name] = std::move(value);
		if (options_.allexport) exported_.insert(name);
	}

	void Environment::setIndexedArrayFromList(const std::string& name,
			std::vector<std::string> values) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		vars_.erase(target);
		assoc_.erase(target);

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
		vars_.erase(target);
		assoc_.erase(target);
		indexed_[target] = std::move(elems);
	}

	void Environment::setIndexedElement(const std::string& name, long long idx, std::string val) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;
		if (!applyAttributes(target, val)) return;

		if (assoc_.count(target) != 0) {
			assoc_[target][std::to_string(idx)] = std::move(val);
			return;
		}

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

	void Environment::declareAssocArray(const std::string& name) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;

		vars_.erase(target);
		indexed_.erase(target);
		assoc_.emplace(target, AssocArray{});
	}

	void Environment::setAssocElement(const std::string& name, std::string key, std::string val) {
		const std::string& target = resolveRef(name);
		if (rejectIfReadonly(target)) return;
		if (!applyAttributes(target, val)) return;

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
		if (indexed != indexed_.end()) indexed->second.erase(idx);
	}

	// Unsetting a nameref unsets the variable it names; one with no
	// target goes itself.
	void Environment::unset(const std::string& name) {
		const std::string target = resolveRef(name);
		if (target == name) namerefs_.erase(name);

		ElementRef element;
		if (splitElementRef(target, element)) {
			unsetElement(element.base, element.index, element.key);
			return;
		}

		vars_.erase(target);
		indexed_.erase(target);
		assoc_.erase(target);
		exported_.erase(target);
		attributes_.erase(target);
	}

	bool Environment::has(const std::string& name) const {
		const std::string& target = resolveRef(name);
		ElementRef element;
		if (splitElementRef(target, element)) return hasElement(element);
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
		if (splitElementRef(target, element)) return getElement(element);
		return getResolved(target);
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
		const std::string current = get(name);
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
		if (state.attributes == 0) attributes_.erase(name);
		else attributes_[name] = state.attributes;
	}

	void Environment::setPositional(std::vector<std::string> args) {
		positional_ = std::move(args);
	}

	unsigned int Environment::randomNext() {
		random_state_ = random_state_ * kRandomMultiplier + kRandomIncrement;
		return (random_state_ / kRandomDivisor) % kRandomRange;
	}

	void Environment::reseedRandom() {
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		random_state_ = randomNext() ^ static_cast<unsigned int>(ticks);
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
		vars_[name] = std::move(value);
		exported_.insert(std::move(name));
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

}  // namespace wbsh
