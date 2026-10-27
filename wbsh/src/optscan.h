#pragma once

/**
 * @file optscan.h
 * @brief getopt_long-style option scanning for the bundled coreutils.
 *
 * Short clusters (`-cd`), attached or separate values (`-f1`, `-f 1`),
 * `--long`, `--long=value` and `--long value`, unique-prefix abbreviation
 * of long names, `--` and GNU operand permutation. Unknown options and
 * missing values are reported with GNU's wording and make the scan fail.
 */

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace wbsh {

	enum class OptArg { None, Required, Optional };

	struct OptDef {
		int id;
		char short_name;
		const char* long_name;
		OptArg arg;
	};

	struct OptHit {
		int id = 0;
		bool has_value = false;
		std::string value;
	};

	struct OptScanResult {
		std::vector<OptHit> hits;
		std::vector<std::string> operands;
	};

	namespace optscan_detail {

		struct Scan {
			const char* tool;
			const std::vector<std::string>& args;
			const std::vector<OptDef>& defs;
			OptScanResult& out;
			std::size_t index = 0;
		};

		inline bool fail(const Scan& scan, const std::string& message) {
			std::fprintf(stderr, "wbsh: %s: %s\n", scan.tool, message.c_str());
			return false;
		}

		inline const OptDef* findShort(const Scan& scan, char name) {
			for (const OptDef& def : scan.defs) {
				if (def.short_name != 0 && def.short_name == name) return &def;
			}

			return nullptr;
		}

		inline const OptDef* findLong(const Scan& scan, const std::string& name, bool& ambiguous) {
			const OptDef* match = nullptr;
			ambiguous = false;
			for (const OptDef& def : scan.defs) {
				if (def.long_name == nullptr) continue;

				const std::string candidate = def.long_name;
				if (candidate == name) return &def;
				if (candidate.compare(0, name.size(), name) != 0) continue;

				if (match != nullptr && match->id != def.id) ambiguous = true;
				match = &def;
			}

			return ambiguous ? nullptr : match;
		}

		inline void addHit(Scan& scan, int id, bool has_value, const std::string& value) {
			OptHit hit;
			hit.id = id;
			hit.has_value = has_value;
			hit.value = value;
			scan.out.hits.push_back(hit);
		}

		inline bool scanShortValue(Scan& scan, const OptDef& def, const std::string& rest) {
			if (!rest.empty() || def.arg == OptArg::Optional) {
				addHit(scan, def.id, !rest.empty(), rest);
				return true;
			}

			if (scan.index + 1 >= scan.args.size()) {
				return fail(scan, std::string("option requires an argument -- '")
					+ def.short_name + "'");
			}

			addHit(scan, def.id, true, scan.args[++scan.index]);
			return true;
		}

		inline bool scanCluster(Scan& scan) {
			const std::string& cluster = scan.args[scan.index];
			for (std::size_t k = 1; k < cluster.size(); ++k) {
				const OptDef* def = findShort(scan, cluster[k]);
				if (def == nullptr) {
					return fail(scan, std::string("invalid option -- '") + cluster[k] + "'");
				}

				if (def->arg != OptArg::None) {
					return scanShortValue(scan, *def, cluster.substr(k + 1));
				}

				addHit(scan, def->id, false, std::string());
			}

			return true;
		}

		inline bool scanLongValue(Scan& scan, const OptDef& def, const std::string& name,
				std::size_t eq, const std::string& body) {
			if (eq != std::string::npos) {
				addHit(scan, def.id, true, body.substr(eq + 1));
				return true;
			}

			if (def.arg == OptArg::Optional) {
				addHit(scan, def.id, false, std::string());
				return true;
			}

			if (scan.index + 1 >= scan.args.size()) {
				return fail(scan, "option '--" + name + "' requires an argument");
			}

			addHit(scan, def.id, true, scan.args[++scan.index]);
			return true;
		}

		inline bool scanLong(Scan& scan) {
			const std::string body = scan.args[scan.index].substr(2);
			const std::size_t eq = body.find('=');
			const std::string name = body.substr(0, eq);

			bool ambiguous = false;
			const OptDef* def = findLong(scan, name, ambiguous);
			if (ambiguous) return fail(scan, "option '--" + name + "' is ambiguous");
			if (def == nullptr) return fail(scan, "unrecognized option '--" + name + "'");

			const std::string full = def->long_name;
			if (def->arg != OptArg::None) return scanLongValue(scan, *def, full, eq, body);
			if (eq != std::string::npos) {
				return fail(scan, "option '--" + full + "' doesn't allow an argument");
			}

			addHit(scan, def->id, false, std::string());
			return true;
		}

		inline void takeRest(Scan& scan, std::size_t from) {
			for (std::size_t i = from; i < scan.args.size(); ++i) {
				scan.out.operands.push_back(scan.args[i]);
			}
		}

		inline bool isOption(const std::string& arg) {
			return arg.size() > 1 && arg[0] == '-';
		}

	}  // namespace optscan_detail

	/// With @p stop_at_operand the first operand ends option scanning (xargs,
	/// where everything after it belongs to the command); otherwise options
	/// and operands may be interleaved.
	inline bool scanOptions(const char* tool, const std::vector<std::string>& args,
			const std::vector<OptDef>& defs, OptScanResult& out, bool stop_at_operand = false) {
		optscan_detail::Scan scan{ tool, args, defs, out };
		for (; scan.index < args.size(); ++scan.index) {
			const std::string& arg = args[scan.index];
			if (arg == "--") {
				optscan_detail::takeRest(scan, scan.index + 1);
				return true;
			}

			if (!optscan_detail::isOption(arg)) {
				if (stop_at_operand) {
					optscan_detail::takeRest(scan, scan.index);
					return true;
				}

				out.operands.push_back(arg);
				continue;
			}

			const bool ok = arg[1] == '-'
				? optscan_detail::scanLong(scan)
				: optscan_detail::scanCluster(scan);
			if (!ok) return false;
		}

		return true;
	}

}  // namespace wbsh
