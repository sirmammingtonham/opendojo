#include "subsystems.hpp"

#include <cstdint>

#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "practice_state.hpp"
#include "signatures.hpp"

namespace {

// Polaris-side pool init function — historically at RVA 0x18E8E00
// in v3.00.02, now resolved via AOB scan (see signatures.cpp).
// Takes the recording subsystem (resolved via KEY_RECORDING) as `this`.
// Writes [recording+0x24]=0, allocates pool1+pool2 if null, memsets them.
// Idempotent.
using PoolInitFn = void (*)(void* this_ptr);

}  // anonymous namespace

std::uintptr_t opendojo::subsystems::lookup(std::uint32_t hash) {
    // Never substitute a stale RVA when code-based discovery fails.
    const auto ctx_slot = signatures::ctx_ptr_addr();
    std::uint64_t ctx = 0, map = 0, sentinel = 0, mask = 0, buckets = 0;
    if (!memory::try_read_u64(ctx_slot, &ctx) || !ctx || !memory::try_read_u64(ctx + 0x10, &map) ||
        !map || !memory::try_read_u64(map + 0x100, &sentinel) || !sentinel ||
        !memory::try_read_u64(map + 0x128, &mask) || !memory::try_read_u64(map + 0x110, &buckets) ||
        !buckets)
        return 0;
    // Bucket counts are powers of two. Bound patch-sensitive address arithmetic.
    if (mask > 0xFFFF || (mask & (mask + 1)) != 0 || buckets > UINTPTR_MAX - (mask + 1) * 0x10)
        return 0;
    const auto bucket = buckets + (mask & hash) * 0x10;
    std::uint64_t first = 0, entry = 0;
    if (!memory::try_read_u64(bucket, &first) || !memory::try_read_u64(bucket + 8, &entry))
        return 0;
    for (int steps = 0; entry && entry != sentinel && steps < 64; ++steps) {
        std::uint32_t key = 0;
        std::uint64_t value = 0;
        if (!memory::try_read_u32(entry + 0x10, &key)) return 0;
        if (key == hash) {
            return memory::try_read_u64(entry + 0x18, &value) ? value : 0;
        }
        if (entry == first) break;
        if (!memory::try_read_u64(entry + 8, &entry)) return 0;
    }
    return 0;
}

bool opendojo::subsystems::in_practice() {
    // Delegates to practice_state, which polls the practice-controller
    // slot (and drives autosave on the entry transition).
    return practice_state::is_active();
}

std::uintptr_t opendojo::subsystems::pool1() {
    return memory::read_u64(signatures::pool1_ptr_addr());
}

std::uintptr_t opendojo::subsystems::pool2() {
    return memory::read_u64(signatures::pool2_ptr_addr());
}

bool opendojo::subsystems::mark_session_loaded(bool loaded) {
    auto singleton = lookup(KEY_SINGLETON);
    if (!singleton) {
        OPENDOJO_LOG("mark_session_loaded: singleton subsystem unresolved");
        return false;
    }

    // Mirror the post-finalize singleton/recording state from the
    // natural Record→Confirm flow (FUN_141911380).
    auto word0 = memory::read_u32(singleton);
    if (loaded) {
        memory::write_u32(singleton,
                          word0 | 0x400000u);  // bit 22: "session exists" — UI gates on this
        memory::write_u32(singleton + 0x22,
                          0u);  // "actively recording" — clear to mark "saved, idle"
        memory::write_u8(singleton + 0x99, 0u);  // mid-record progress flag — clear when done
        auto recording = lookup(KEY_RECORDING);
        if (recording)
            memory::write_u32(
                recording + 0x28,
                0u);  // pre_clear's 2nd write; meaning unknown but natural finalize zeroes it
    } else {
        memory::write_u32(singleton, word0 & ~0x400000u);  // clear "session exists" bit
    }

    // opponent_player[0x39C0]: "this opponent has a recording session
    // loaded for playback". Practice UI gates the playback option on it.
    // Reached via the GlobalPlayerHolder chain (KEY_PLAYERS_SUB not
    // reliably resolved in our context).
    auto opponent = players::cpu_player_address();
    if (!opponent) {
        OPENDOJO_LOG("mark_session_loaded: opponent player address unresolved");
        return false;
    }
    memory::write_u32(opponent + 0x39C0, loaded ? 1u : 0u);
    return true;
}

void opendojo::subsystems::ensure_pool_allocated() {
    if (memory::read_u64(signatures::pool1_ptr_addr()) != 0) return;

    // Pass the real recording subsystem as `this` (not a stack dummy) so
    // pool_init's `[this+0x24] = 0` clear lands on the right object. We
    // skip the post_init follow-up the natural caller does — it was a
    // speculative fix that ended up touching recording[0x64], [0x5c] in
    // ways the in-game UI didn't like.
    auto recording = lookup(KEY_RECORDING);
    if (!recording) {
        OPENDOJO_LOG("subsystems: KEY_RECORDING unresolved — skipping forced alloc");
        return;
    }

    auto pool_init_addr = signatures::pool_init();
    if (!pool_init_addr) {
        OPENDOJO_LOG("subsystems: pool_init signature unresolved — skipping forced alloc");
        return;
    }
    auto pool_init = reinterpret_cast<PoolInitFn>(pool_init_addr);
    pool_init(reinterpret_cast<void*>(recording));

    auto p1 = memory::read_u64(signatures::pool1_ptr_addr());
    auto p2 = memory::read_u64(signatures::pool2_ptr_addr());
    OPENDOJO_LOG("subsystems: force-allocated pool1=0x%llX pool2=0x%llX (recording=0x%llX)",
                 static_cast<unsigned long long>(p1), static_cast<unsigned long long>(p2),
                 static_cast<unsigned long long>(recording));
}
