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

namespace wbsh {

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
		vars_["IFS"] = " \t\n";
		const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
		random_state_ = static_cast<unsigned int>(shell_pid_) ^ static_cast<unsigned int>(ticks);
	}

	bool Environment::rejectIfReadonly(const std::string& name) const {
		if (readonly_.count(name) == 0) return false;

		std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
		return true;
	}

	void Environment::set(const std::string& name, std::string value) {
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

		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) {
			indexed->second[0] = std::move(value);
			return;
		}

		assoc_.erase(name);
		vars_[name] = std::move(value);
	}

	void Environment::setIndexedArrayFromList(const std::string& name,
			std::vector<std::string> values) {
		if (rejectIfReadonly(name)) return;

		vars_.erase(name);
		assoc_.erase(name);

		IndexedArray array;
		for (std::size_t i = 0; i < values.size(); ++i) {
			array[static_cast<long long>(i)] = std::move(values[i]);
		}

		indexed_[name] = std::move(array);
	}

	void Environment::setIndexedArraySparse(const std::string& name,
			std::map<long long, std::string> elems) {
		if (rejectIfReadonly(name)) return;

		vars_.erase(name);
		assoc_.erase(name);
		indexed_[name] = std::move(elems);
	}

	void Environment::setIndexedElement(const std::string& name, long long idx, std::string val) {
		if (rejectIfReadonly(name)) return;

		if (assoc_.count(name) != 0) {
			assoc_[name][std::to_string(idx)] = std::move(val);
			return;
		}

		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) {
			indexed->second[idx] = std::move(val);
			return;
		}

		IndexedArray array;
		auto scalar = vars_.find(name);
		if (scalar != vars_.end()) {
			array[0] = std::move(scalar->second);
			vars_.erase(scalar);
		}

		array[idx] = std::move(val);
		indexed_[name] = std::move(array);
	}

	void Environment::declareAssocArray(const std::string& name) {
		if (rejectIfReadonly(name)) return;

		vars_.erase(name);
		indexed_.erase(name);
		assoc_.emplace(name, AssocArray{});
	}

	void Environment::setAssocElement(const std::string& name, std::string key, std::string val) {
		if (rejectIfReadonly(name)) return;

		auto assoc = assoc_.find(name);
		if (assoc != assoc_.end()) {
			assoc->second[std::move(key)] = std::move(val);
			return;
		}

		vars_.erase(name);
		indexed_.erase(name);
		assoc_[name][std::move(key)] = std::move(val);
	}

	void Environment::unsetElement(const std::string& name, long long idx, const std::string& key) {
		if (rejectIfReadonly(name)) return;

		auto assoc = assoc_.find(name);
		if (assoc != assoc_.end()) {
			assoc->second.erase(key);
			return;
		}

		auto indexed = indexed_.find(name);
		if (indexed != indexed_.end()) indexed->second.erase(idx);
	}

	void Environment::unset(const std::string& name) {
		vars_.erase(name);
		indexed_.erase(name);
		assoc_.erase(name);
		exported_.erase(name);
	}

	bool Environment::has(const std::string& name) const {
		if (vars_.find(name) != vars_.end()) return true;
		if (indexed_.count(name) != 0) return true;
		if (assoc_.count(name) != 0) return true;
		return false;
	}

	std::string Environment::get(const std::string& name) const {
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
		exported_.insert(name);
	}

	void Environment::unexportVar(const std::string& name) {
		exported_.erase(name);
	}

	bool Environment::isExported(const std::string& name) const {
		return exported_.count(name) != 0;
	}

	void Environment::setPositional(std::vector<std::string> args) {
		positional_ = std::move(args);
	}

	unsigned int Environment::randomNext() {
		random_state_ = random_state_ * kRandomMultiplier + kRandomIncrement;
		return (random_state_ / kRandomDivisor) % kRandomRange;
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
