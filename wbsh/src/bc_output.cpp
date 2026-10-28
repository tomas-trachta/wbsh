#include "bc_internal.h"

#include <cstdio>

namespace wbsh {

	namespace bc_detail {

		// GNU bc wraps before the column that would reach line_length - 1,
		// leaving room for the trailing backslash.
		void BcOutput::put(char c) {
			if (c == '\n') {
				column = 0;
				pending.push_back(c);
				return;
			}

			++column;
			if (line_length > 0 && column == line_length - 1) {
				pending += "\\\n";
				column = 1;
			}

			pending.push_back(c);
		}

		void BcOutput::write(const std::string& text) {
			for (char c : text) put(c);
		}

		void BcOutput::flush() {
			if (!pending.empty()) std::fwrite(pending.data(), 1, pending.size(), stdout);
			pending.clear();
			std::fflush(stdout);
		}

		static void writeDecimal(BcOutput& out, const BcNum& value) {
			std::string digits = natToDecimal(value.mag);
			const auto scale = static_cast<std::size_t>(value.scale);
			if (digits.size() <= scale) digits.insert(0, scale - digits.size(), '0');

			const std::size_t whole = digits.size() - scale;
			out.write(digits.substr(0, whole));
			if (scale == 0) return;

			out.put('.');
			out.write(digits.substr(whole));
		}

		static int decimalWidth(std::int64_t value) {
			return static_cast<int>(std::to_string(value).size());
		}

		static void writeDigit(BcOutput& out, std::uint32_t digit, std::int64_t obase, bool space) {
			if (obase <= 16) {
				out.put("0123456789ABCDEF"[digit]);
				return;
			}

			if (space) out.put(' ');
			const std::string text = std::to_string(digit);
			const int width = decimalWidth(obase - 1);
			for (int pad = static_cast<int>(text.size()); pad < width; ++pad) out.put('0');
			out.write(text);
		}

		static void writeWholeInBase(BcOutput& out, BigNat whole, std::int64_t obase) {
			std::vector<std::uint32_t> digits;
			while (!natIsZero(whole)) digits.push_back(natDivSmall(whole, static_cast<std::uint32_t>(obase)));

			for (auto it = digits.rbegin(); it != digits.rend(); ++it) writeDigit(out, *it, obase, true);
		}

		// As in GNU bc: emit fraction digits until obase^n has more decimal
		// digits than the number's scale.
		static void writeFractionInBase(BcOutput& out, BigNat fraction, std::int64_t scale, std::int64_t obase) {
			const BigNat unit = natPow10(scale);
			BigNat reach = natFromU64(1);
			bool first = true;

			while (natDigitCount(reach) <= scale) {
				natMulSmall(fraction, static_cast<std::uint32_t>(obase), 0);

				BigNat digit;
				BigNat rest;
				natDivMod(fraction, unit, digit, rest);
				fraction = std::move(rest);

				writeDigit(out, natIsZero(digit) ? 0 : digit.limbs[0], obase, !first);
				first = false;
				natMulSmall(reach, static_cast<std::uint32_t>(obase), 0);
			}
		}

		static void writeInBase(BcOutput& out, const BcNum& value, std::int64_t obase) {
			const BigNat whole = natShiftDown(value.mag, value.scale);
			writeWholeInBase(out, whole, obase);
			if (value.scale == 0) return;

			out.put('.');
			const BigNat fraction = natSub(value.mag, natShiftUp(whole, value.scale));
			writeFractionInBase(out, fraction, value.scale, obase);
		}

		void writeNumber(BcOutput& out, const BcNum& value, std::int64_t obase) {
			if (bcIsZero(value)) {
				out.put('0');
				return;
			}

			if (value.negative) out.put('-');
			if (obase == 10) writeDecimal(out, value);
			else writeInBase(out, value, obase);
		}

	}  // namespace bc_detail

}  // namespace wbsh
