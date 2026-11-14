#pragma once

/**
 * @file assocmap.h
 * @brief The container behind associative arrays.
 *
 * Scripts see the order in which `${!a[@]}`, `${a[@]}` and `declare -p`
 * list the keys, so it has to be bash's: bash 5.3 keeps an associative
 * array in a chained hash table (FNV-1, 1024 buckets, four times as many
 * once it holds two entries per bucket) and walks it bucket by bucket,
 * newest key first within a bucket.
 */

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

namespace wbsh {

	namespace assocmap_detail {
		/// bash's hash_string (FNV-1), which orders its hash tables.
		std::uint32_t bashHashString(const std::string& key);
	}

	class BashAssocMap {
	public:
		static const std::uint32_t kInitialBuckets = 1024;

		using value_type     = std::pair<const std::string, std::string>;
		using iterator       = std::list<value_type>::iterator;
		using const_iterator = std::list<value_type>::const_iterator;
		using const_reverse_iterator = std::list<value_type>::const_reverse_iterator;

		BashAssocMap() = default;
		BashAssocMap(const BashAssocMap& other);
		BashAssocMap(BashAssocMap&& other) noexcept = default;
		BashAssocMap& operator=(const BashAssocMap& other);
		BashAssocMap& operator=(BashAssocMap&& other) noexcept = default;

		iterator begin() { return items_.begin(); }
		iterator end() { return items_.end(); }
		const_iterator begin() const { return items_.begin(); }
		const_iterator end() const { return items_.end(); }
		const_reverse_iterator rbegin() const { return items_.rbegin(); }
		const_reverse_iterator rend() const { return items_.rend(); }

		std::size_t size() const { return items_.size(); }
		bool empty() const { return items_.empty(); }
		std::size_t count(const std::string& key) const { return slots_.count(key); }

		iterator find(const std::string& key);
		const_iterator find(const std::string& key) const;

		std::string& operator[](const std::string& key);
		std::size_t erase(const std::string& key);
		void clear();

		bool operator==(const BashAssocMap& other) const { return items_ == other.items_; }
		bool operator!=(const BashAssocMap& other) const { return !(*this == other); }

	private:
		struct Slot {
			iterator item;
			std::uint32_t bucket = 0;
		};

		std::list<value_type> items_;
		std::unordered_map<std::string, Slot> slots_;
		std::map<std::uint32_t, iterator> bucket_heads_;
		std::uint32_t buckets_ = kInitialBuckets;

		std::uint32_t bucketOf(const std::string& key) const;
		iterator insertNew(const std::string& key, std::string value);
		iterator firstItemAfterBucket(std::uint32_t bucket);
		void unlinkFromBucket(const Slot& slot);
		void growIfFull();
		void copyFrom(const BashAssocMap& other);
	};

}  // namespace wbsh
