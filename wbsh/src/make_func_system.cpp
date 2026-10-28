/**
 * @file make_func_system.cpp
 * @brief The functions that reach outside make: shell, file, wildcard,
 *        realpath and abspath.
 */

#include "make_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "coreutils_internal.h"
#include "executor.h"
#include "fnmatch.h"
#include "pathconv.h"

namespace wbsh::make_detail {

	namespace fs = std::filesystem;

	std::string shellOutputValue(std::string output) {
		output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
		while (!output.empty() && output.back() == '\n') output.pop_back();
		std::replace(output.begin(), output.end(), '\n', ' ');
		return output;
	}

	void functionShell(FunctionCall& call, std::string& out) {
		std::string output;
		int status = 0;
		captureShell(call.make, call.args[0], call.ctx, output, status);
		setGlobal(call.make, ".SHELLSTATUS", std::to_string(status), Origin::Override);
		out += shellOutputValue(output);
	}

	static bool writeTextFile(FunctionCall& call, const std::string& name, const char* mode) {
		std::FILE* file = fopenNative(call.make.exec, name, mode);
		if (file == nullptr) {
			reportFatal(call.make, call.make.where,
				"open: " + name + ": " + std::strerror(errno));
			return false;
		}

		if (call.args.size() > 1) {
			const std::string& text = call.args[1];
			std::fwrite(text.data(), 1, text.size(), file);
			if (text.empty() || text.back() != '\n') std::fputc('\n', file);
		}

		std::fclose(file);
		return true;
	}

	static void readTextFile(FunctionCall& call, const std::string& name, std::string& out) {
		if (call.args.size() > 1) {
			reportFatal(call.make, call.make.where, "file: too many arguments");
			return;
		}

		std::FILE* file = fopenNative(call.make.exec, name, "rb");
		if (file == nullptr) return;

		std::string text;
		char buffer[4096];
		std::size_t got = 0;
		while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
		std::fclose(file);

		if (!text.empty() && text.back() == '\n') text.pop_back();
		if (!text.empty() && text.back() == '\r') text.pop_back();
		out += text;
	}

	void functionFile(FunctionCall& call, std::string& out) {
		const std::string spec = trimBlanks(call.args[0]);
		const std::size_t op_length = spec.compare(0, 2, ">>") == 0 ? 2 : 1;
		const std::string op = spec.substr(0, op_length);
		const std::string name = trimBlanks(spec.substr((std::min)(op_length, spec.size())));
		if (op != ">>" && op != ">" && op != "<") {
			reportFatal(call.make, call.make.where, "file: invalid file operation: " + spec);
			return;
		}

		if (name.empty()) {
			reportFatal(call.make, call.make.where, "file: missing filename");
			return;
		}

		if (op == "<") {
			readTextFile(call, name, out);
			return;
		}

		writeTextFile(call, name, op == ">>" ? "ab" : "wb");
	}

	static std::string joinPath(const std::string& base, const std::string& name) {
		if (base.empty()) return name;
		if (base.back() == '/') return base + name;

		return base + "/" + name;
	}

	static std::vector<std::string> matchDirectory(Make& make, const std::string& base,
	                                               const std::string& component) {
		std::vector<std::string> matches;
		std::error_code ec;
		const fs::path dir = toNative(make.exec, base.empty() ? "." : base);
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
			const std::string name = pathToUtf8(it->path().filename());
			if (name.empty() || (name[0] == '.' && component[0] != '.')) continue;
			if (fnmatchFull(component, name)) matches.push_back(joinPath(base, name));
		}

		return matches;
	}

	struct GlobWalk {
		Make& make;
		const std::vector<std::string>& components;
		std::vector<std::string>& found;
	};

	static void globFrom(GlobWalk& walk, std::size_t index, const std::string& base) {
		if (index == walk.components.size()) {
			if (fileExists(walk.make, base)) walk.found.push_back(base);
			return;
		}

		const std::string& component = walk.components[index];
		if (!hasGlobMeta(component)) {
			globFrom(walk, index + 1, joinPath(base, component));
			return;
		}

		for (const std::string& match : matchDirectory(walk.make, base, component)) {
			globFrom(walk, index + 1, match);
		}
	}

	static std::string globRoot(const std::string& pattern) {
		if (!pattern.empty() && pattern[0] == '/') return "/";
		if (pattern.size() >= 3 && pattern[1] == ':' && pattern[2] == '/') return pattern.substr(0, 3);

		return {};
	}

	static std::vector<std::string> pathComponents(const std::string& path) {
		std::vector<std::string> components;
		std::size_t start = 0;
		while (start <= path.size()) {
			std::size_t slash = path.find('/', start);
			if (slash == std::string::npos) slash = path.size();
			if (slash > start) components.push_back(path.substr(start, slash - start));
			start = slash + 1;
		}

		return components;
	}

	std::vector<std::string> globWord(Make& make, const std::string& pattern) {
		if (!hasGlobMeta(pattern)) {
			if (fileExists(make, pattern)) return { pattern };
			return {};
		}

		const std::string root = globRoot(pattern);
		const std::vector<std::string> components = pathComponents(pattern.substr(root.size()));
		std::vector<std::string> found;
		GlobWalk walk{ make, components, found };
		globFrom(walk, 0, root);
		std::sort(found.begin(), found.end());
		return found;
	}

	void functionWildcard(FunctionCall& call, std::string& out) {
		std::vector<std::string> results;
		for (const std::string& pattern : splitWords(call.args[0])) {
			for (std::string& match : globWord(call.make, pattern)) results.push_back(std::move(match));
		}

		out += joinWords(results);
	}

	void functionRealpath(FunctionCall& call, std::string& out) {
		std::vector<std::string> results;
		for (const std::string& word : splitWords(call.args[0])) {
			std::error_code ec;
			const fs::path resolved = fs::canonical(toNative(call.make.exec, word), ec);
			if (ec) continue;

			const std::string native = pathToUtf8(withoutExtendedPrefix(resolved));
			results.push_back(call.make.exec.pathConv().toPosix(native));
		}

		out += joinWords(results);
	}

	static std::string normalizeAbsolute(const std::string& path) {
		std::vector<std::string> kept;
		for (const std::string& component : pathComponents(path)) {
			if (component == ".") continue;
			if (component == "..") {
				if (!kept.empty()) kept.pop_back();
				continue;
			}

			kept.push_back(component);
		}

		std::string normalized;
		for (const std::string& component : kept) normalized += "/" + component;
		return normalized.empty() ? "/" : normalized;
	}

	static std::string absoluteForm(Make& make, const std::string& word) {
		if (PathConv::isWin32Absolute(word)) return make.exec.pathConv().toPosix(word);
		if (word[0] == '/') return word;

		return rawVariableValue(make, "CURDIR", ExpandContext{}) + "/" + word;
	}

	void functionAbspath(FunctionCall& call, std::string& out) {
		std::vector<std::string> results;
		for (const std::string& word : splitWords(call.args[0])) {
			results.push_back(normalizeAbsolute(absoluteForm(call.make, word)));
		}

		out += joinWords(results);
	}

}  // namespace wbsh::make_detail
