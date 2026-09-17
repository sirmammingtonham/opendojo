#pragma once

#include <cstring>
#include <vector>

#include "memory.hpp"

namespace opendojo {

// Validate the whole operation before its first mutation. This is not a game
// transaction: another thread can still destroy an object during commit.
class WriteBatch {
public:
    enum class Result { Ok, Rejected, PartialWrite };

    bool add(std::uintptr_t address, const void* data, std::size_t size) {
        if (!data || !size || !memory::is_writable(address, size)) return valid_ = false;
        Entry entry{address, std::vector<std::uint8_t>(size), std::vector<std::uint8_t>(size)};
        if (!memory::try_read_bytes(address, entry.before.data(), size)) return valid_ = false;
        std::memcpy(entry.after.data(), data, size);
        entries_.push_back(std::move(entry));
        return true;
    }

    template <typename T>
    bool add(std::uintptr_t address, T value) {
        return add(address, &value, sizeof(value));
    }

    template <typename T>
    void expect(std::uintptr_t address, T value) {
        Entry entry{address, std::vector<std::uint8_t>(sizeof(T)), {}};
        std::memcpy(entry.before.data(), &value, sizeof(T));
        entries_.push_back(std::move(entry));
    }

    Result commit() {
        if (!valid_ || consumed_) return Result::Rejected;
        consumed_ = true;
        // Allocate and perform every check before writing anything.
        for (const auto& entry : entries_) {
            std::vector<std::uint8_t> current(entry.before.size());
            if ((!entry.after.empty() && !memory::is_writable(entry.address, entry.after.size())) ||
                !memory::try_read_bytes(entry.address, current.data(), current.size()) ||
                current != entry.before)
                return Result::Rejected;
        }
        bool wrote = false;
        for (const auto& entry : entries_) {
            if (entry.after.empty()) continue;
            if (!memory::try_write_bytes(entry.address, entry.after.data(), entry.after.size())) {
                // memcpy can fault after copying a prefix, even on the first write.
                return Result::PartialWrite;
            }
            wrote = true;
        }
        return wrote ? Result::Ok : Result::Rejected;
    }

private:
    struct Entry {
        std::uintptr_t address;
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
    };
    std::vector<Entry> entries_;
    bool valid_ = true;
    bool consumed_ = false;
};

}  // namespace opendojo
