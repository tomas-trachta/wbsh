#pragma once

/**
 * @file bcnum.h
 * @brief Arbitrary precision decimal numbers for bc.
 *
 * A BcNum is `mag * 10^-scale` with a sign; `mag` is an unsigned big integer
 * in base 10^9 limbs. The operations follow GNU bc's scale rules.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace wbsh {

	namespace bc_detail {

		inline constexpr std::uint32_t kLimbBase = 1000000000u;
		inline constexpr int kLimbDigits = 9;

		/// Little-endian base 10^9 limbs without high zero limbs; zero is empty.
		struct BigNat {
			std::vector<std::uint32_t> limbs;
		};

		bool natIsZero(const BigNat& a);
		int natCompare(const BigNat& a, const BigNat& b);
		BigNat natFromU64(std::uint64_t value);
		BigNat natAdd(const BigNat& a, const BigNat& b);
		BigNat natSub(const BigNat& a, const BigNat& b);
		BigNat natMul(const BigNat& a, const BigNat& b);
		void natMulSmall(BigNat& a, std::uint32_t factor, std::uint32_t addend);
		std::uint32_t natDivSmall(BigNat& a, std::uint32_t divisor);
		void natDivMod(const BigNat& a, const BigNat& b, BigNat& quotient, BigNat& remainder);
		BigNat natPow10(std::int64_t exponent);
		BigNat natShiftUp(const BigNat& a, std::int64_t digits);
		BigNat natShiftDown(const BigNat& a, std::int64_t digits);
		BigNat natSqrt(const BigNat& a);
		std::int64_t natDigitCount(const BigNat& a);
		std::string natToDecimal(const BigNat& a);
		BigNat natFromDecimal(const std::string& digits);

		struct BcNum {
			BigNat mag;
			std::int64_t scale = 0;
			bool negative = false;
		};

		BcNum bcFromInt(long long value);
		bool bcIsZero(const BcNum& a);
		int bcCompare(const BcNum& a, const BcNum& b);
		BcNum bcNegate(BcNum a);
		BcNum bcAbs(BcNum a);
		BcNum bcRescale(const BcNum& a, std::int64_t scale);
		BcNum bcIntegerPart(const BcNum& a);
		BcNum bcAdd(const BcNum& a, const BcNum& b);
		BcNum bcSub(const BcNum& a, const BcNum& b);
		BcNum bcMul(const BcNum& a, const BcNum& b, std::int64_t scale);
		BcNum bcMulExact(const BcNum& a, const BcNum& b);
		bool bcDiv(const BcNum& a, const BcNum& b, std::int64_t scale, BcNum& out);
		bool bcMod(const BcNum& a, const BcNum& b, std::int64_t scale, BcNum& out);
		bool bcSqrt(const BcNum& a, std::int64_t scale, BcNum& out);
		bool bcToInt64(const BcNum& a, std::int64_t& out);
		std::int64_t bcIntegerDigits(const BcNum& a);
		std::int64_t bcLength(const BcNum& a);
		BcNum bcFromLiteral(const std::string& text, int base);

		enum class PowResult { Ok, DivideByZero, ExponentTooLarge };
		PowResult bcPow(const BcNum& base, const BcNum& exponent, std::int64_t scale, BcNum& out);

	}  // namespace bc_detail

}  // namespace wbsh
