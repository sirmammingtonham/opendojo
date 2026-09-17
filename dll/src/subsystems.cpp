#include "subsystems.hpp"

#include <cstdint>

#include "log.hpp"
#include "memory.hpp"
#include "players.hpp"
#include "practice_state.hpp"
#include "signatures.hpp"
#include "game_thread.hpp"
#include "write_batch.hpp"

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
    const auto layout = signatures::subsystem_layout();
    if (!layout.bucket_stride) return 0;
    const auto ctx_slot = signatures::ctx_ptr_addr();
    std::uint64_t ctx = 0, map = 0, sentinel = 0, mask = 0, buckets = 0;
    if (!memory::try_read_u64(ctx_slot, &ctx) || !ctx || ctx > UINTPTR_MAX - layout.map ||
        !memory::try_read_u64(ctx + layout.map, &map) || !map || map > UINTPTR_MAX - 0x1008 ||
        !memory::try_read_u64(map + layout.sentinel, &sentinel) || !sentinel ||
        !memory::try_read_u64(map + layout.mask, &mask) ||
        !memory::try_read_u64(map + layout.buckets, &buckets) || !buckets)
        return 0;
    // Bucket counts are powers of two. Bound patch-sensitive address arithmetic.
    if (mask > 0xFFFF || (mask & (mask + 1)) != 0 ||
        buckets > UINTPTR_MAX - (mask + 1) * layout.bucket_stride)
        return 0;
    const auto bucket = buckets + (mask & hash) * layout.bucket_stride;
    std::uint64_t first = 0, entry = 0;
    if (!memory::try_read_u64(bucket, &first) ||
        !memory::try_read_u64(bucket + layout.bucket_last, &entry))
        return 0;
    for (int steps = 0; entry && entry != sentinel && steps < 64; ++steps) {
        if (entry > UINTPTR_MAX - 0x100) return 0;
        std::uint32_t key = 0;
        std::uint64_t value = 0;
        if (!memory::try_read_u32(entry + layout.key, &key)) return 0;
        if (key == hash) {
            return memory::try_read_u64(entry + layout.value, &value) ? value : 0;
        }
        if (entry == first) break;
        if (!memory::try_read_u64(entry + layout.previous, &entry)) return 0;
    }
    return 0;
}

bool opendojo::subsystems::in_practice() {
    // Delegates to practice_state, which polls the practice-controller
    // slot (and drives autosave on the entry transition).
    return practice_state::is_active();
}

std::uintptr_t opendojo::subsystems::pool1() {
    if (!signatures::live_recordings_supported()) return 0;
    return memory::read_u64(signatures::pool1_ptr_addr());
}

std::uintptr_t opendojo::subsystems::pool2() {
    return memory::read_u64(signatures::pool2_ptr_addr());
}

bool opendojo::subsystems::mark_session_loaded(bool loaded) {
    if (!game_thread::is_current()) return false;
    const auto layout = signatures::session_layout();
    const auto recording_layout = signatures::recording_state_layout();
    if (!layout.player_flag || !recording_layout.recording_state) return false;
    const auto singleton = lookup(KEY_SINGLETON);
    const auto opponent = players::cpu_player_address();
    const auto recording = loaded ? lookup(KEY_RECORDING) : 0;
    if (!singleton || !opponent || (loaded && !recording)) {
        OPENDOJO_LOG("mark_session_loaded: required subsystem/player unresolved; nothing written");
        return false;
    }
    std::uint32_t word0 = 0;
    if (!memory::try_read_u32(singleton, &word0)) return false;
    WriteBatch batch;
    batch.expect(singleton, word0);
    batch.add(singleton, loaded ? word0 | layout.active_mask : word0 & ~layout.active_mask);
    if (loaded) {
        batch.add(singleton + layout.pending, std::uint32_t{0});
        batch.add(singleton + layout.finished, std::uint8_t{0});
        batch.add(recording + recording_layout.recording_state, std::uint32_t{0});
    }
    batch.add(opponent + layout.player_flag, loaded ? 1u : 0u);
    if (singleton != lookup(KEY_SINGLETON) || opponent != players::cpu_player_address() ||
        (loaded && recording != lookup(KEY_RECORDING)))
        return false;
    const auto result = batch.commit();
    if (result != WriteBatch::Result::Ok) {
        OPENDOJO_LOG("mark_session_loaded: %s",
                     result == WriteBatch::Result::PartialWrite
                         ? "memory changed during writes; state may be partial"
                         : "preflight rejected; nothing written");
    }
    return result == WriteBatch::Result::Ok;
}

void opendojo::subsystems::ensure_pool_allocated() {
    if (!game_thread::is_current()) return;
    if (!signatures::live_recordings_supported()) return;
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
