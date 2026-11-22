/**
 * @file assocmap.cpp
 * @brief bash 5.3's hash-table order for associative arrays.
 */

#include "assocmap.h"

#include <iterator>
#include <vector>

namespace wbsh::assocmap_detail {

	static const std::uint32_t kFnvOffset = 2166136261u;
	static const std::uint32_t kFnvPrime = 16777619u;
	static const std::size_t kEntriesPerBucket = 2;
	static const std::uint32_t kGrowthFactor = 4;

	// bash's hash_string reads the key through a plain (signed) char, so
	// a byte from 0x80 up is sign-extended before it is folded in.
	std::uint32_t bashHashString(const std::string& key) {
		std::uint32_t hash = kFnvOffset;
		for (const char c : key) {
			hash *= kFnvPrime;
			hash ^= static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<signed char>(c)));
		}

		return hash;
	}

}  // namespace wbsh::assocmap_detail

namespace wbsh {

	using namespace assocmap_detail;

	BashAssocMap::BashAssocMap(const BashAssocMap& other) {
		copyFrom(other);
	}

	BashAssocMap& BashAssocMap::operator=(const BashAssocMap& other) {
		if (this == &other) return *this;

		clear();
		copyFrom(other);
		return *this;
	}

	void BashAssocMap::copyFrom(const BashAssocMap& other) {
		buckets_ = other.buckets_;
		for (const value_type& item : other.items_) {
			items_.push_back(item);
			const iterator inserted = std::prev(items_.end());
			const std::uint32_t bucket = bucketOf(item.first);
			slots_[item.first] = Slot{ inserted, bucket };
			bucket_heads_.emplace(bucket, inserted);
		}
	}

	std::uint32_t BashAssocMap::bucketOf(const std::string& key) const {
		return bashHashString(key) & (buckets_ - 1);
	}

	BashAssocMap::iterator BashAssocMap::find(const std::string& key) {
		const auto found = slots_.find(key);
		return found == slots_.end() ? items_.end() : found->second.item;
	}

	BashAssocMap::const_iterator BashAssocMap::find(const std::string& key) const {
		const auto found = slots_.find(key);
		return found == slots_.end() ? items_.end() : const_iterator(found->second.item);
	}

	std::string& BashAssocMap::operator[](const std::string& key) {
		const auto found = slots_.find(key);
		if (found != slots_.end()) return found->second.item->second;

		growIfFull();
		return insertNew(key, std::string())->second;
	}

	BashAssocMap::iterator BashAssocMap::firstItemAfterBucket(std::uint32_t bucket) {
		const auto next = bucket_heads_.upper_bound(bucket);
		return next == bucket_heads_.end() ? items_.end() : next->second;
	}

	// A new key goes to the head of its bucket's chain.
	BashAssocMap::iterator BashAssocMap::insertNew(const std::string& key, std::string value) {
		const std::uint32_t bucket = bucketOf(key);
		const auto head = bucket_heads_.find(bucket);
		const iterator before = head != bucket_heads_.end() ? head->second
			: firstItemAfterBucket(bucket);

		const iterator inserted = items_.emplace(before, key, std::move(value));
		bucket_heads_[bucket] = inserted;
		slots_[key] = Slot{ inserted, bucket };
		return inserted;
	}

	// Rehashing walks the old table in order and pushes each entry onto
	// the head of its new chain, as bash's hash_rehash does.
	void BashAssocMap::growIfFull() {
		if (items_.size() < static_cast<std::size_t>(buckets_) * kEntriesPerBucket) return;

		std::vector<std::pair<std::string, std::string>> old;
		old.reserve(items_.size());
		for (value_type& item : items_) old.emplace_back(item.first, std::move(item.second));

		const std::uint32_t grown = buckets_ * kGrowthFactor;
		clear();
		buckets_ = grown;
		for (auto& item : old) insertNew(item.first, std::move(item.second));
	}

	void BashAssocMap::unlinkFromBucket(const Slot& slot) {
		const auto head = bucket_heads_.find(slot.bucket);
		if (head == bucket_heads_.end() || head->second != slot.item) return;

		const iterator next = std::next(slot.item);
		const bool next_shares_bucket = next != items_.end()
			&& slots_.at(next->first).bucket == slot.bucket;
		if (next_shares_bucket) head->second = next;
		else                    bucket_heads_.erase(head);
	}

	std::size_t BashAssocMap::erase(const std::string& key) {
		const auto found = slots_.find(key);
		if (found == slots_.end()) return 0;

		unlinkFromBucket(found->second);
		items_.erase(found->second.item);
		slots_.erase(found);
		return 1;
	}

	// As bash's hash_flush, the table keeps its size.
	void BashAssocMap::clear() {
		items_.clear();
		slots_.clear();
		bucket_heads_.clear();
	}

}  // namespace wbsh
