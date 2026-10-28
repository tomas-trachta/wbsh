#include "bcnum.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace wbsh {

	namespace bc_detail {

		static const std::uint32_t kSmallPow10[kLimbDigits + 1] = {
			1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u, 10000000u, 100000000u, 1000000000u,
		};

		static void trimNat(BigNat& a) {
			while (!a.limbs.empty() && a.limbs.back() == 0) a.limbs.pop_back();
		}

		bool natIsZero(const BigNat& a) {
			return a.limbs.empty();
		}

		int natCompare(const BigNat& a, const BigNat& b) {
			if (a.limbs.size() != b.limbs.size()) return a.limbs.size() < b.limbs.size() ? -1 : 1;

			for (std::size_t i = a.limbs.size(); i-- > 0;) {
				if (a.limbs[i] != b.limbs[i]) return a.limbs[i] < b.limbs[i] ? -1 : 1;
			}

			return 0;
		}

		BigNat natFromU64(std::uint64_t value) {
			BigNat result;
			while (value != 0) {
				result.limbs.push_back(static_cast<std::uint32_t>(value % kLimbBase));
				value /= kLimbBase;
			}

			return result;
		}

		BigNat natAdd(const BigNat& a, const BigNat& b) {
			const BigNat& longer = a.limbs.size() >= b.limbs.size() ? a : b;
			const BigNat& shorter = a.limbs.size() >= b.limbs.size() ? b : a;

			BigNat result;
			result.limbs.reserve(longer.limbs.size() + 1);

			std::uint32_t carry = 0;
			for (std::size_t i = 0; i < longer.limbs.size(); ++i) {
				std::uint32_t sum = longer.limbs[i] + carry;
				if (i < shorter.limbs.size()) sum += shorter.limbs[i];
				carry = sum >= kLimbBase ? 1u : 0u;
				result.limbs.push_back(sum - carry * kLimbBase);
			}

			if (carry != 0) result.limbs.push_back(carry);
			return result;
		}

		BigNat natSub(const BigNat& a, const BigNat& b) {
			BigNat result = a;

			std::int64_t borrow = 0;
			for (std::size_t i = 0; i < result.limbs.size(); ++i) {
				std::int64_t diff = static_cast<std::int64_t>(result.limbs[i]) - borrow;
				if (i < b.limbs.size()) diff -= b.limbs[i];
				borrow = diff < 0 ? 1 : 0;
				result.limbs[i] = static_cast<std::uint32_t>(diff + borrow * kLimbBase);
			}

			trimNat(result);
			return result;
		}

		BigNat natMul(const BigNat& a, const BigNat& b) {
			if (natIsZero(a) || natIsZero(b)) return {};

			BigNat result;
			result.limbs.assign(a.limbs.size() + b.limbs.size(), 0);

			for (std::size_t i = 0; i < a.limbs.size(); ++i) {
				std::uint64_t carry = 0;
				for (std::size_t j = 0; j < b.limbs.size(); ++j) {
					const std::uint64_t cur = result.limbs[i + j]
						+ static_cast<std::uint64_t>(a.limbs[i]) * b.limbs[j] + carry;
					result.limbs[i + j] = static_cast<std::uint32_t>(cur % kLimbBase);
					carry = cur / kLimbBase;
				}

				result.limbs[i + b.limbs.size()] = static_cast<std::uint32_t>(carry);
			}

			trimNat(result);
			return result;
		}

		void natMulSmall(BigNat& a, std::uint32_t factor, std::uint32_t addend) {
			std::uint64_t carry = addend;
			for (auto& limb : a.limbs) {
				const std::uint64_t cur = static_cast<std::uint64_t>(limb) * factor + carry;
				limb = static_cast<std::uint32_t>(cur % kLimbBase);
				carry = cur / kLimbBase;
			}

			while (carry != 0) {
				a.limbs.push_back(static_cast<std::uint32_t>(carry % kLimbBase));
				carry /= kLimbBase;
			}

			trimNat(a);
		}

		std::uint32_t natDivSmall(BigNat& a, std::uint32_t divisor) {
			std::uint64_t remainder = 0;
			for (std::size_t i = a.limbs.size(); i-- > 0;) {
				const std::uint64_t cur = remainder * kLimbBase + a.limbs[i];
				a.limbs[i] = static_cast<std::uint32_t>(cur / divisor);
				remainder = cur % divisor;
			}

			trimNat(a);
			return static_cast<std::uint32_t>(remainder);
		}

		static std::uint64_t estimateQuotientDigit(const std::vector<std::uint32_t>& u,
				const std::vector<std::uint32_t>& v, std::size_t j) {
			const std::size_t n = v.size();
			const std::uint64_t top = static_cast<std::uint64_t>(u[j + n]) * kLimbBase + u[j + n - 1];
			std::uint64_t qhat = top / v[n - 1];
			std::uint64_t rhat = top % v[n - 1];

			while (qhat >= kLimbBase || qhat * v[n - 2] > rhat * kLimbBase + u[j + n - 2]) {
				--qhat;
				rhat += v[n - 1];
				if (rhat >= kLimbBase) break;
			}

			return qhat;
		}

		static bool multiplySubtract(std::vector<std::uint32_t>& u, const std::vector<std::uint32_t>& v,
				std::size_t j, std::uint64_t qhat) {
			const std::size_t n = v.size();
			std::uint64_t carry = 0;
			std::int64_t borrow = 0;

			for (std::size_t i = 0; i < n; ++i) {
				const std::uint64_t product = qhat * v[i] + carry;
				carry = product / kLimbBase;
				std::int64_t diff = static_cast<std::int64_t>(u[i + j])
					- static_cast<std::int64_t>(product % kLimbBase) - borrow;
				borrow = diff < 0 ? 1 : 0;
				u[i + j] = static_cast<std::uint32_t>(diff + borrow * kLimbBase);
			}

			const std::int64_t top = static_cast<std::int64_t>(u[j + n])
				- static_cast<std::int64_t>(carry) - borrow;
			u[j + n] = static_cast<std::uint32_t>(top < 0 ? top + kLimbBase : top);
			return top < 0;
		}

		static void addBack(std::vector<std::uint32_t>& u, const std::vector<std::uint32_t>& v, std::size_t j) {
			const std::size_t n = v.size();
			std::uint32_t carry = 0;
			for (std::size_t i = 0; i < n; ++i) {
				std::uint32_t sum = u[i + j] + v[i] + carry;
				carry = sum >= kLimbBase ? 1u : 0u;
				u[i + j] = sum - carry * kLimbBase;
			}

			u[j + n] = (u[j + n] + carry) % kLimbBase;
		}

		static void knuthDivide(const BigNat& a, const BigNat& b, BigNat& quotient, BigNat& remainder) {
			const std::size_t n = b.limbs.size();
			const std::size_t m = a.limbs.size() - n;
			const std::uint32_t norm = kLimbBase / (b.limbs[n - 1] + 1);

			BigNat u = a;
			natMulSmall(u, norm, 0);
			u.limbs.resize(a.limbs.size() + 1, 0);

			BigNat v = b;
			natMulSmall(v, norm, 0);

			quotient.limbs.assign(m + 1, 0);
			for (std::size_t j = m + 1; j-- > 0;) {
				std::uint64_t qhat = estimateQuotientDigit(u.limbs, v.limbs, j);
				if (multiplySubtract(u.limbs, v.limbs, j, qhat)) {
					--qhat;
					addBack(u.limbs, v.limbs, j);
				}

				quotient.limbs[j] = static_cast<std::uint32_t>(qhat);
			}

			trimNat(quotient);
			u.limbs.resize(n);
			trimNat(u);
			natDivSmall(u, norm);
			remainder = std::move(u);
		}

		void natDivMod(const BigNat& a, const BigNat& b, BigNat& quotient, BigNat& remainder) {
			if (natCompare(a, b) < 0) {
				quotient = {};
				remainder = a;
				return;
			}

			if (b.limbs.size() == 1) {
				quotient = a;
				remainder = natFromU64(natDivSmall(quotient, b.limbs[0]));
				return;
			}

			knuthDivide(a, b, quotient, remainder);
		}

		BigNat natPow10(std::int64_t exponent) {
			BigNat result;
			result.limbs.assign(static_cast<std::size_t>(exponent / kLimbDigits), 0);
			result.limbs.push_back(kSmallPow10[exponent % kLimbDigits]);
			return result;
		}

		BigNat natShiftUp(const BigNat& a, std::int64_t digits) {
			if (natIsZero(a) || digits <= 0) return a;

			BigNat result;
			result.limbs.assign(static_cast<std::size_t>(digits / kLimbDigits), 0);
			result.limbs.insert(result.limbs.end(), a.limbs.begin(), a.limbs.end());
			natMulSmall(result, kSmallPow10[digits % kLimbDigits], 0);
			return result;
		}

		BigNat natShiftDown(const BigNat& a, std::int64_t digits) {
			if (digits <= 0) return a;
			if (digits >= natDigitCount(a)) return {};

			BigNat result;
			const auto skipped = static_cast<std::ptrdiff_t>(digits / kLimbDigits);
			result.limbs.assign(a.limbs.begin() + skipped, a.limbs.end());
			natDivSmall(result, kSmallPow10[digits % kLimbDigits]);
			return result;
		}

		BigNat natSqrt(const BigNat& a) {
			if (natIsZero(a)) return {};

			BigNat x = natPow10((natDigitCount(a) + 1) / 2);
			for (;;) {
				BigNat quotient;
				BigNat remainder;
				natDivMod(a, x, quotient, remainder);

				BigNat next = natAdd(x, quotient);
				natDivSmall(next, 2);
				if (natCompare(next, x) >= 0) return x;
				x = std::move(next);
			}
		}

		static int smallDigitCount(std::uint32_t value) {
			int count = 1;
			while (count < kLimbDigits && value >= kSmallPow10[count]) ++count;
			return count;
		}

		std::int64_t natDigitCount(const BigNat& a) {
			if (natIsZero(a)) return 0;
			const auto full = static_cast<std::int64_t>(a.limbs.size() - 1) * kLimbDigits;
			return full + smallDigitCount(a.limbs.back());
		}

		std::string natToDecimal(const BigNat& a) {
			if (natIsZero(a)) return "0";

			std::string text = std::to_string(a.limbs.back());
			char chunk[16];
			for (std::size_t i = a.limbs.size() - 1; i-- > 0;) {
				std::snprintf(chunk, sizeof(chunk), "%09u", static_cast<unsigned>(a.limbs[i]));
				text += chunk;
			}

			return text;
		}

		BigNat natFromDecimal(const std::string& digits) {
			BigNat result;
			std::size_t end = digits.size();
			while (end > 0) {
				const std::size_t begin = end >= kLimbDigits ? end - kLimbDigits : 0;
				std::uint32_t limb = 0;
				for (std::size_t i = begin; i < end; ++i) {
					limb = limb * 10 + static_cast<std::uint32_t>(digits[i] - '0');
				}

				result.limbs.push_back(limb);
				end = begin;
			}

			trimNat(result);
			return result;
		}

		static void normalizeSign(BcNum& a) {
			if (natIsZero(a.mag)) a.negative = false;
		}

		BcNum bcFromInt(long long value) {
			BcNum result;
			result.negative = value < 0;
			const std::uint64_t magnitude = value < 0
				? 0 - static_cast<std::uint64_t>(value) : static_cast<std::uint64_t>(value);
			result.mag = natFromU64(magnitude);
			return result;
		}

		bool bcIsZero(const BcNum& a) {
			return natIsZero(a.mag);
		}

		static BigNat alignedMag(const BcNum& a, std::int64_t scale) {
			return natShiftUp(a.mag, scale - a.scale);
		}

		int bcCompare(const BcNum& a, const BcNum& b) {
			if (a.negative != b.negative) return a.negative ? -1 : 1;

			const std::int64_t scale = std::max(a.scale, b.scale);
			const int cmp = natCompare(alignedMag(a, scale), alignedMag(b, scale));
			return a.negative ? -cmp : cmp;
		}

		BcNum bcNegate(BcNum a) {
			a.negative = !a.negative;
			normalizeSign(a);
			return a;
		}

		BcNum bcAbs(BcNum a) {
			a.negative = false;
			return a;
		}

		BcNum bcRescale(const BcNum& a, std::int64_t scale) {
			BcNum result;
			result.negative = a.negative;
			result.scale = scale;
			result.mag = scale >= a.scale ? natShiftUp(a.mag, scale - a.scale) : natShiftDown(a.mag, a.scale - scale);
			normalizeSign(result);
			return result;
		}

		BcNum bcIntegerPart(const BcNum& a) {
			return bcRescale(a, 0);
		}

		BcNum bcAdd(const BcNum& a, const BcNum& b) {
			const std::int64_t scale = std::max(a.scale, b.scale);
			const BigNat left = alignedMag(a, scale);
			const BigNat right = alignedMag(b, scale);

			BcNum result;
			result.scale = scale;
			if (a.negative == b.negative) {
				result.mag = natAdd(left, right);
				result.negative = a.negative;
			} else if (natCompare(left, right) >= 0) {
				result.mag = natSub(left, right);
				result.negative = a.negative;
			} else {
				result.mag = natSub(right, left);
				result.negative = b.negative;
			}

			normalizeSign(result);
			return result;
		}

		BcNum bcSub(const BcNum& a, const BcNum& b) {
			return bcAdd(a, bcNegate(b));
		}

		BcNum bcMulExact(const BcNum& a, const BcNum& b) {
			BcNum result;
			result.mag = natMul(a.mag, b.mag);
			result.scale = a.scale + b.scale;
			result.negative = a.negative != b.negative;
			normalizeSign(result);
			return result;
		}

		BcNum bcMul(const BcNum& a, const BcNum& b, std::int64_t scale) {
			const std::int64_t full = a.scale + b.scale;
			const std::int64_t kept = std::min(full, std::max({ scale, a.scale, b.scale }));
			return bcRescale(bcMulExact(a, b), kept);
		}

		bool bcDiv(const BcNum& a, const BcNum& b, std::int64_t scale, BcNum& out) {
			if (bcIsZero(b)) return false;

			const std::int64_t shift = scale + b.scale - a.scale;
			const BigNat numerator = shift >= 0 ? natShiftUp(a.mag, shift) : natShiftDown(a.mag, -shift);

			BigNat remainder;
			natDivMod(numerator, b.mag, out.mag, remainder);
			out.scale = scale;
			out.negative = a.negative != b.negative;
			normalizeSign(out);
			return true;
		}

		bool bcMod(const BcNum& a, const BcNum& b, std::int64_t scale, BcNum& out) {
			BcNum quotient;
			if (!bcDiv(a, b, scale, quotient)) return false;

			out = bcSub(a, bcMulExact(quotient, b));
			return true;
		}

		bool bcSqrt(const BcNum& a, std::int64_t scale, BcNum& out) {
			if (a.negative) return false;
			if (bcIsZero(a) || bcCompare(a, bcFromInt(1)) == 0) {
				out = bcFromInt(bcIsZero(a) ? 0 : 1);
				return true;
			}

			const std::int64_t kept = std::max(scale, a.scale);
			out.mag = natSqrt(natShiftUp(a.mag, 2 * kept - a.scale));
			out.scale = kept;
			out.negative = false;
			return true;
		}

		bool bcToInt64(const BcNum& a, std::int64_t& out) {
			const BigNat whole = natShiftDown(a.mag, a.scale);
			std::uint64_t value = 0;
			for (std::size_t i = whole.limbs.size(); i-- > 0;) {
				if (value > (std::numeric_limits<std::uint64_t>::max() - whole.limbs[i]) / kLimbBase) return false;
				value = value * kLimbBase + whole.limbs[i];
			}

			const auto limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
			if (value > limit) return false;

			out = a.negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
			return true;
		}

		std::int64_t bcIntegerDigits(const BcNum& a) {
			return std::max<std::int64_t>(0, natDigitCount(a.mag) - a.scale);
		}

		std::int64_t bcLength(const BcNum& a) {
			if (bcIsZero(a)) return std::max<std::int64_t>(1, a.scale);
			return natDigitCount(a.mag);
		}

		static std::uint32_t literalDigit(char c) {
			if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
			return static_cast<std::uint32_t>(c - 'A' + 10);
		}

		static BigNat digitsInBase(const std::string& digits, int base) {
			const auto top = static_cast<std::uint32_t>(base - 1);
			BigNat value;
			for (char c : digits) natMulSmall(value, static_cast<std::uint32_t>(base), std::min(literalDigit(c), top));
			return value;
		}

		static BcNum fractionInBase(const std::string& digits, int base) {
			BcNum numerator;
			numerator.mag = digitsInBase(digits, base);

			BcNum denominator = bcFromInt(1);
			const auto factor = static_cast<std::uint32_t>(base);
			for (std::size_t i = 0; i < digits.size(); ++i) natMulSmall(denominator.mag, factor, 0);

			BcNum result;
			bcDiv(numerator, denominator, static_cast<std::int64_t>(digits.size()), result);
			return result;
		}

		BcNum bcFromLiteral(const std::string& text, int base) {
			if (text.size() == 1) return bcFromInt(literalDigit(text[0]));

			const std::size_t dot = text.find('.');
			const std::string whole = text.substr(0, dot);
			const std::string fraction = dot == std::string::npos ? std::string() : text.substr(dot + 1);

			BcNum result;
			result.mag = digitsInBase(whole, base);
			if (fraction.empty()) return result;

			return bcAdd(result, fractionInBase(fraction, base));
		}

		static BigNat natPower(BigNat base, std::uint64_t exponent) {
			BigNat result = natFromU64(1);
			while (exponent != 0) {
				if ((exponent & 1u) != 0) result = natMul(result, base);
				exponent >>= 1;
				if (exponent != 0) base = natMul(base, base);
			}

			return result;
		}

		static std::int64_t powerScale(std::int64_t base_scale, std::int64_t exponent, std::int64_t scale) {
			const std::int64_t cap = std::max(scale, base_scale);
			if (base_scale != 0 && exponent > cap / base_scale) return cap;
			return std::min(base_scale * exponent, cap);
		}

		PowResult bcPow(const BcNum& base, const BcNum& exponent, std::int64_t scale, BcNum& out) {
			std::int64_t power = 0;
			if (!bcToInt64(exponent, power)) return PowResult::ExponentTooLarge;
			if (power == 0) {
				out = bcFromInt(1);
				return PowResult::Ok;
			}

			const auto raw = static_cast<std::uint64_t>(power);
			const std::uint64_t count = power < 0 ? 0 - raw : raw;
			const std::int64_t largest = std::numeric_limits<std::int64_t>::max();
			if (base.scale != 0 && count > static_cast<std::uint64_t>(largest / base.scale)) {
				return PowResult::ExponentTooLarge;
			}

			BcNum exact;
			exact.mag = natPower(base.mag, count);
			exact.scale = base.scale * static_cast<std::int64_t>(count);
			exact.negative = base.negative && (count & 1u) != 0;
			normalizeSign(exact);

			if (power > 0) {
				out = bcRescale(exact, powerScale(base.scale, power, scale));
				return PowResult::Ok;
			}

			return bcDiv(bcFromInt(1), exact, scale, out) ? PowResult::Ok : PowResult::DivideByZero;
		}

	}  // namespace bc_detail

}  // namespace wbsh
