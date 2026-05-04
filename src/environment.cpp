#include "environment.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
extern char** environ;
#endif

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace wbsh {

	Environment::Environment() {
#ifdef _WIN32
		shell_pid_ = static_cast<long long>(::GetCurrentProcessId());
#else
		shell_pid_ = static_cast<long long>(::getpid());
#endif
		// Provide a sane default IFS, matching POSIX.
		vars_["IFS"] = " \t\n";
		// Seed the RANDOM LCG from PID + time so different shells get
		// different streams.
		random_state_ = static_cast<unsigned int>(shell_pid_)
		    ^ static_cast<unsigned int>(
		        std::chrono::steady_clock::now().time_since_epoch().count());
	}

	void Environment::set(const std::string& name, std::string value) {
		// Dynamic special parameters: assignment is a state poke, not a
		// stored value. Reads always synthesise.
		if (name == "RANDOM") {
			try { setRandomSeed(static_cast<unsigned int>(std::stoul(value))); }
			catch (...) {}
			return;
		}
		if (name == "SECONDS") {
			try { setSecondsOffset(std::stoll(value)); } catch (...) {}
			return;
		}
		if (name == "LINENO" || name == "BASHPID") {
			// Read-only dynamic params; ignore writes silently.
			return;
		}
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		// Plain scalar assignment removes any array form bash-style:
		//   `arr=foo` to a previous indexed array sets index 0 only,
		//   keeping the rest. We keep the simpler interpretation: if
		//   there's an existing indexed array, set element 0; otherwise
		//   normal scalar.
		auto it = indexed_.find(name);
		if (it != indexed_.end()) {
			it->second[0] = std::move(value);
			return;
		}
		assoc_.erase(name);
		vars_[name] = std::move(value);
	}

	void Environment::setIndexedArrayFromList(const std::string& name,
	                                          std::vector<std::string> values) {
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		vars_.erase(name);
		assoc_.erase(name);
		IndexedArray ia;
		for (std::size_t i = 0; i < values.size(); ++i) {
			ia[static_cast<long long>(i)] = std::move(values[i]);
		}
		indexed_[name] = std::move(ia);
	}

	void Environment::setIndexedArraySparse(const std::string& name,
	                                        std::map<long long, std::string> elems) {
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		vars_.erase(name);
		assoc_.erase(name);
		indexed_[name] = std::move(elems);
	}

	void Environment::setIndexedElement(const std::string& name, long long idx,
	                                    std::string val) {
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		// Promote a scalar to an indexed array on first element write.
		if (assoc_.count(name)) {
			// Treat assoc-keyed numeric assignment as assoc[key=str(idx)].
			assoc_[name][std::to_string(idx)] = std::move(val);
			return;
		}
		auto it = indexed_.find(name);
		if (it == indexed_.end()) {
			IndexedArray ia;
			auto sv = vars_.find(name);
			if (sv != vars_.end()) {
				ia[0] = std::move(sv->second);
				vars_.erase(sv);
			}
			ia[idx] = std::move(val);
			indexed_[name] = std::move(ia);
			return;
		}
		it->second[idx] = std::move(val);
	}

	void Environment::declareAssocArray(const std::string& name) {
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		vars_.erase(name);
		indexed_.erase(name);
		assoc_.emplace(name, AssocArray{});
	}

	void Environment::setAssocElement(const std::string& name, std::string key,
	                                  std::string val) {
		if (readonly_.count(name)) {
			std::fprintf(stderr, "wbsh: %s: readonly variable\n", name.c_str());
			return;
		}
		auto it = assoc_.find(name);
		if (it == assoc_.end()) {
			vars_.erase(name);
			indexed_.erase(name);
			assoc_[name][std::move(key)] = std::move(val);
			return;
		}
		it->second[std::move(key)] = std::move(val);
	}

	void Environment::unset(const std::string& name) {
		vars_.erase(name);
		indexed_.erase(name);
		assoc_.erase(name);
		exported_.erase(name);
	}

	bool Environment::has(const std::string& name) const {
		if (vars_.find(name) != vars_.end()) return true;
		if (indexed_.count(name)) return true;
		if (assoc_.count(name)) return true;
		return false;
	}

	std::string Environment::get(const std::string& name) const {
		auto it = vars_.find(name);
		if (it != vars_.end()) return it->second;
		// $arr without subscript means $arr[0].
		auto ix = indexed_.find(name);
		if (ix != indexed_.end()) {
			auto e = ix->second.find(0);
			return e == ix->second.end() ? std::string() : e->second;
		}
		auto as = assoc_.find(name);
		if (as != assoc_.end()) {
			auto e = as->second.find("0");
			return e == as->second.end() ? std::string() : e->second;
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
		// Classic LCG; mask to 0..32767 to match bash.
		random_state_ = random_state_ * 1103515245u + 12345u;
		return (random_state_ / 65536u) % 32768u;
	}

	long long Environment::secondsSinceStart() const {
		auto now = std::chrono::steady_clock::now();
		auto secs = std::chrono::duration_cast<std::chrono::seconds>(
			now - start_time_).count();
		return static_cast<long long>(secs) - seconds_offset_;
	}

	void Environment::setSecondsOffset(long long s) {
		auto now = std::chrono::steady_clock::now();
		auto secs = std::chrono::duration_cast<std::chrono::seconds>(
			now - start_time_).count();
		seconds_offset_ = static_cast<long long>(secs) - s;
	}

	void Environment::loadFromProcessEnv() {
#ifdef _WIN32
		LPWCH block = ::GetEnvironmentStringsW();
		if (!block) return;
		for (LPWCH p = block; *p; ) {
			std::wstring entry = p;
			p += entry.size() + 1;
			auto eq = entry.find(L'=');
			if (eq == std::wstring::npos || eq == 0) continue;
			std::wstring wname = entry.substr(0, eq);
			std::wstring wval  = entry.substr(eq + 1);
			// Convert to UTF-8 (best-effort using WideCharToMultiByte).
			auto toUtf8 = [](const std::wstring& w) -> std::string {
				if (w.empty()) return {};
				int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
											  nullptr, 0, nullptr, nullptr);
				std::string s(n, '\0');
				::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
									  s.data(), n, nullptr, nullptr);
				return s;
			};
			std::string name = toUtf8(wname);
			std::string val  = toUtf8(wval);
			vars_[name] = std::move(val);
			exported_.insert(std::move(name));
		}
		::FreeEnvironmentStringsW(block);
#else
		for (char** p = environ; *p; ++p) {
			std::string entry = *p;
			auto eq = entry.find('=');
			if (eq == std::string::npos || eq == 0) continue;
			std::string name = entry.substr(0, eq);
			std::string val  = entry.substr(eq + 1);
			vars_[name] = std::move(val);
			exported_.insert(std::move(name));
		}
#endif
	}

}  // namespace wbsh
