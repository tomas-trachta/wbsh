#pragma once

/**
 * @file arena.h
 * @brief Bump-pointer memory arena for node-heavy structures (ASTs).
 *
 * Objects are allocated by bumping an offset inside large chunks, so a
 * whole tree costs a handful of mallocs instead of one per node, nodes
 * sit contiguously in memory, and teardown is one pass instead of a
 * recursive ownership-pointer chain. Destructors of non-trivially-
 * destructible objects are recorded at make() time and run (in reverse
 * allocation order) when the arena is destroyed — so members like
 * std::string / std::vector inside arena objects release normally.
 *
 * Arenas are move-only: whoever holds the Arena owns every object ever
 * made from it. Pointers returned by make() are stable for the arena's
 * lifetime and are deliberately raw — they are borrows, not owners.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace wbsh {

	class Arena {
	public:
		Arena() = default;
		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;
		// Moved-from arenas are valid and empty: allocate() re-checks
		// chunks_ before trusting the bump offset.
		Arena(Arena&&) = default;
		Arena& operator=(Arena&& other) {
			if (this != &other) {
				destroyAll();
				chunks_ = std::move(other.chunks_);
				dtors_ = std::move(other.dtors_);
				used_ = other.used_;
				cap_ = other.cap_;
			}
			return *this;
		}
		~Arena() { destroyAll(); }

		/// Returns a borrowed pointer, valid for the arena's lifetime.
		template <typename T, typename... Args>
		T* make(Args&&... args) {
			// Chunks come from new[] and are aligned to the default new
			// alignment; over-aligned types would need aligned chunks.
			static_assert(alignof(T) <= alignof(std::max_align_t),
			              "Arena does not support over-aligned types");
			void* mem = allocate(sizeof(T), alignof(T));
			T* obj = new (mem) T(std::forward<Args>(args)...);
			if constexpr (!std::is_trivially_destructible_v<T>) {
				dtors_.push_back({ obj, [](void* p) {
					static_cast<T*>(p)->~T();
				} });
			}
			return obj;
		}

	private:
		struct Dtor {
			void* obj;
			void (*fn)(void*);
		};

		static constexpr std::size_t kChunkSize = 16 * 1024;

		void* allocate(std::size_t size, std::size_t align) {
			std::size_t off = (used_ + align - 1) & ~(align - 1);
			if (chunks_.empty() || off + size > cap_) {
				const std::size_t need = size + align;
				cap_ = need > kChunkSize ? need : kChunkSize;
				chunks_.push_back(std::make_unique<unsigned char[]>(cap_));
				const auto base = reinterpret_cast<std::uintptr_t>(
					chunks_.back().get());
				off = ((base + align - 1) & ~(align - 1)) - base;
			}
			used_ = off + size;
			return chunks_.back().get() + off;
		}

		// Reverse order so later objects (which may reference earlier
		// ones) are destroyed first, mirroring stack unwind order.
		void destroyAll() {
			for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it) {
				it->fn(it->obj);
			}
			dtors_.clear();
			chunks_.clear();
			used_ = 0;
			cap_ = 0;
		}

		std::vector<std::unique_ptr<unsigned char[]>> chunks_;
		std::vector<Dtor> dtors_;
		std::size_t used_ = 0;
		std::size_t cap_ = 0;
	};

}  // namespace wbsh
