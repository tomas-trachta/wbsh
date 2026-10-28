#include "bc_internal.h"

#include <algorithm>

namespace wbsh {

	namespace bc_detail {

		// Extra digits carried through each series so that the result,
		// truncated to the caller's scale, is exact in practice.
		static const std::int64_t kGuardDigits = 10;
		static const std::int64_t kMagnitudeCap = 100000000;

		static BcNum quotient(const BcNum& a, const BcNum& b, std::int64_t scale) {
			BcNum out;
			bcDiv(a, b, scale, out);
			return out;
		}

		static BcNum product(const BcNum& a, const BcNum& b, std::int64_t scale) {
			return bcRescale(bcMulExact(a, b), std::min(a.scale + b.scale, scale));
		}

		static BcNum fromDecimal(const char* text) {
			return bcFromLiteral(text, 10);
		}

		static std::int64_t magnitude(const BcNum& x) {
			std::int64_t value = 0;
			if (!bcToInt64(bcAbs(x), value) || value > kMagnitudeCap) return kMagnitudeCap;
			return value;
		}

		static BcNum timesPowerOfTwo(const BcNum& x, std::int64_t count) {
			BcNum result = x;
			for (std::int64_t i = 0; i < count; ++i) result = bcAdd(result, result);
			return result;
		}

		static BcNum expSeries(const BcNum& x, std::int64_t work) {
			BcNum sum = bcFromInt(1);
			BcNum term = bcFromInt(1);
			for (long long n = 1;; ++n) {
				term = quotient(product(term, x, work), bcFromInt(n), work);
				if (bcIsZero(term)) return sum;
				sum = bcAdd(sum, term);
			}
		}

		static BcNum mathExp(const BcNum& x, std::int64_t scale) {
			if (x.negative) return quotient(bcFromInt(1), mathExp(bcAbs(x), scale + kGuardDigits), scale);

			const BcNum half = fromDecimal(".5");
			BcNum reduced = x;
			std::int64_t halvings = 0;
			while (bcCompare(reduced, half) > 0) {
				reduced = quotient(reduced, bcFromInt(2), reduced.scale + 1);
				++halvings;
			}

			const std::int64_t work = scale + kGuardDigits + halvings + magnitude(x) * 44 / 100 + 1;
			BcNum sum = expSeries(reduced, work);
			for (std::int64_t i = 0; i < halvings; ++i) sum = product(sum, sum, work);
			return bcRescale(sum, scale);
		}

		static BcNum atanhSeries(const BcNum& u, std::int64_t work) {
			const BcNum square = product(u, u, work);
			BcNum sum = u;
			BcNum power = u;
			for (long long n = 3;; n += 2) {
				power = product(power, square, work);
				if (bcIsZero(power)) return sum;
				sum = bcAdd(sum, quotient(power, bcFromInt(n), work));
			}
		}

		static BcNum mathLn(const BcNum& x, std::int64_t scale) {
			if (x.negative || bcIsZero(x)) {
				BcNum power;
				power.mag = natPow10(scale);
				return bcSub(bcFromInt(1), power);
			}

			const BcNum one = bcFromInt(1);
			if (bcCompare(x, one) < 0) {
				return bcNegate(mathLn(quotient(one, x, scale + kGuardDigits + x.scale), scale));
			}

			const std::int64_t work = scale + kGuardDigits + bcIntegerDigits(x) + 3;
			const BcNum limit = fromDecimal("1.1");
			BcNum reduced = x;
			std::int64_t roots = 0;
			while (bcCompare(reduced, limit) > 0) {
				bcSqrt(reduced, work, reduced);
				++roots;
			}

			const BcNum u = quotient(bcSub(reduced, one), bcAdd(reduced, one), work);
			return bcRescale(timesPowerOfTwo(atanhSeries(u, work), roots + 1), scale);
		}

		static BcNum atanSeries(const BcNum& x, std::int64_t work) {
			const BcNum square = product(x, x, work);
			BcNum sum = x;
			BcNum power = x;
			for (long long n = 3;; n += 2) {
				power = bcNegate(product(power, square, work));
				if (bcIsZero(power)) return sum;
				sum = bcAdd(sum, quotient(power, bcFromInt(n), work));
			}
		}

		// atan(x) for 0 <= x <= 1, halving the angle until the series converges fast.
		static BcNum atanCore(const BcNum& x, std::int64_t work) {
			const BcNum one = bcFromInt(1);
			const BcNum limit = fromDecimal(".2");
			BcNum reduced = x;
			std::int64_t halvings = 0;
			while (bcCompare(reduced, limit) > 0) {
				BcNum root;
				bcSqrt(bcAdd(one, product(reduced, reduced, work)), work, root);
				reduced = quotient(reduced, bcAdd(one, root), work);
				++halvings;
			}

			return timesPowerOfTwo(atanSeries(reduced, work), halvings);
		}

		static BcNum mathAtan(const BcNum& x, std::int64_t scale) {
			if (x.negative) return bcNegate(mathAtan(bcAbs(x), scale));

			const std::int64_t work = scale + kGuardDigits;
			const BcNum one = bcFromInt(1);
			if (bcCompare(x, one) <= 0) return bcRescale(atanCore(x, work), scale);

			const BcNum half_pi = timesPowerOfTwo(atanCore(one, work), 1);
			return bcRescale(bcSub(half_pi, atanCore(quotient(one, x, work), work)), scale);
		}

		static BcNum reduceAngle(const BcNum& x, std::int64_t work) {
			const BcNum pi = timesPowerOfTwo(atanCore(bcFromInt(1), work), 2);
			const BcNum two_pi = bcAdd(pi, pi);

			const BcNum turns = quotient(x, two_pi, 0);
			BcNum angle = bcRescale(bcSub(x, bcMulExact(turns, two_pi)), work);
			if (bcCompare(angle, pi) > 0) angle = bcSub(angle, two_pi);
			if (bcCompare(angle, bcNegate(pi)) < 0) angle = bcAdd(angle, two_pi);
			return angle;
		}

		// Taylor series of sin (first = x, start = 1) or cos (first = 1, start = 0).
		static BcNum trigSeries(const BcNum& angle, BcNum first, long long start, std::int64_t work) {
			const BcNum square = product(angle, angle, work);
			BcNum sum = first;
			BcNum term = std::move(first);
			for (long long n = start;; n += 2) {
				term = quotient(bcNegate(product(term, square, work)), bcFromInt((n + 1) * (n + 2)), work);
				if (bcIsZero(term)) return sum;
				sum = bcAdd(sum, term);
			}
		}

		static BcNum mathSin(const BcNum& x, std::int64_t scale) {
			const std::int64_t work = scale + kGuardDigits + bcIntegerDigits(x);
			const BcNum angle = reduceAngle(x, work);
			return bcRescale(trigSeries(angle, angle, 1, work), scale);
		}

		static BcNum mathCos(const BcNum& x, std::int64_t scale) {
			const std::int64_t work = scale + kGuardDigits + bcIntegerDigits(x);
			const BcNum angle = reduceAngle(x, work);
			return bcRescale(trigSeries(angle, bcFromInt(1), 0, work), scale);
		}

		static BcNum besselFirstTerm(const BcNum& half, std::int64_t order, std::int64_t work) {
			BcNum term = bcFromInt(1);
			for (std::int64_t i = 1; i <= order; ++i) term = quotient(product(term, half, work), bcFromInt(i), work);
			return term;
		}

		static BcNum besselSeries(std::int64_t order, const BcNum& x, std::int64_t work) {
			const BcNum half = quotient(x, bcFromInt(2), std::max(work, x.scale + 1));
			const BcNum square = product(half, half, work);
			const std::int64_t reach = magnitude(x);

			BcNum term = besselFirstTerm(half, order, work);
			BcNum sum = term;
			for (std::int64_t k = 1;; ++k) {
				term = quotient(bcNegate(product(term, square, work)), bcFromInt(k * (k + order)), work);
				if (bcIsZero(term) && k > reach) return sum;
				sum = bcAdd(sum, term);
			}
		}

		static BcNum mathBessel(const BcNum& n, const BcNum& x, std::int64_t scale) {
			std::int64_t order = 0;
			if (!bcToInt64(n, order)) return BcNum();

			const bool flip = order < 0 && (order % 2) != 0;
			order = order < 0 ? -order : order;

			const std::int64_t work = scale + kGuardDigits + magnitude(x) * 44 / 100 + 1;
			const BcNum result = bcRescale(besselSeries(order, x, work), scale);
			return flip ? bcNegate(result) : result;
		}

		bool isMathLibFunction(const std::string& name) {
			return name == "s" || name == "c" || name == "a" || name == "l" || name == "e" || name == "j";
		}

		bool callMathLib(const std::string& name, const std::vector<BcNum>& args, std::int64_t scale, BcNum& out) {
			if (name == "j") {
				if (args.size() != 2) return false;
				out = mathBessel(args[0], args[1], scale);
				return true;
			}

			if (args.size() != 1) return false;

			const BcNum& x = args[0];
			if (name == "s") out = mathSin(x, scale);
			else if (name == "c") out = mathCos(x, scale);
			else if (name == "a") out = mathAtan(x, scale);
			else if (name == "l") out = mathLn(x, scale);
			else out = mathExp(x, scale);
			return true;
		}

	}  // namespace bc_detail

}  // namespace wbsh
