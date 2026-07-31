// Real two-thread stress tests for the lock-free SPSC primitives.
//
// WHY THIS FILE EXISTS
// --------------------
// Until now every one of the 27 test files in this suite was single-threaded, so
// the entire value proposition of spsc_ring.hpp — six explicit
// memory_order_acquire/release annotations — was unfalsifiable by the test
// suite. test_spsc_ring.cpp:2 said as much: "Multi-threaded stress + TSAN
// coverage deferred to P2-CORE-05/BUILD follow-up."
//
// WHAT THESE TESTS CAN AND CANNOT PROVE — read before trusting a green run
// ------------------------------------------------------------------------
// These tests verify the LOGICAL contract across a real thread boundary: no item
// lost, no item duplicated, strict FIFO order, and payload integrity (each item
// carries a checksum over its own fields, so a torn read is detectable rather
// than merely improbable).
//
// They do NOT, on their own, prove the memory orders are correct. On x86-TSO
// every plain load is acquire and every plain store is release at the hardware
// level, so downgrading spsc_ring.hpp's release stores to memory_order_relaxed
// would leave these tests passing on any x86 CI runner. Two things close that
// gap, and neither is optional:
//
//   1. ThreadSanitizer (-DHY_SANITIZER=thread). TSan is a happens-before
//      detector working on the C++ memory model, not on what the hardware
//      happened to do, so it flags a missing release/acquire edge on x86.
//      tsan_control_relaxed_ring.cpp is a deliberately-broken twin that TSan
//      MUST report — if that control ever passes, this file's TSan runs prove
//      nothing and must not be trusted.
//   2. Running this file on real weak-memory hardware (ARM64), where a relaxed
//      downgrade genuinely reorders and the Payload checksum below is what
//      observes it. That is a hardware verdict rather than a model verdict.
//
// GTEST + THREADS DISCIPLINE
// --------------------------
// Never use ASSERT_* on a worker thread: it returns from the LAMBDA, not from
// the test, silently leaving the thread half-run and the test still going. Each
// thread writes its own result struct; all assertions happen after join().
//
// Governance: L1, no network/token/order. GTest only.

#include <gtest/gtest.h>
#include <hengyuan/binance_market_event.hpp>
#include <hengyuan/intent_channel.hpp>
#include <hengyuan/spsc_ring.hpp>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using hy::BinanceMarketEvent;
using hy::IntentChannel;
using hy::SpscRing;
using hy::TimestampedIntent;

namespace {

// Iteration count: large enough that producer and consumer genuinely interleave
// and the ring both fills and drains many times over, small enough that the
// instrumented (TSan/ASan) build stays well inside a CI timeout. TSan slows
// contended atomics by roughly an order of magnitude.
constexpr std::uint64_t kItems = 200'000;

// Ring deliberately much smaller than kItems so the producer hits "full" and the
// consumer hits "empty" repeatedly. A ring big enough to hold everything would
// never exercise the wrap or the backpressure paths — it would look like a test
// and behave like a buffer.
constexpr std::size_t kRingSlots = 64;

// A self-checking payload. Every field is derived from the sequence number, so a
// torn or partially-visible slot is detectable rather than merely unlikely: the
// consumer recomputes the expected values and compares. Without this, a
// half-written slot whose fields happened to be individually plausible would
// pass silently.
// Field types are taken exactly from binance_market_event.hpp — event_id and
// ts_event_ms are uint64_t, price_ticks/qty_lots are int64_t, symbol_id is
// uint32_t. Mixing them up produces signed/unsigned comparison warnings that
// /W4 /WX and -Wsign-compare reject outright.
BinanceMarketEvent make_item(std::uint64_t seq) {
    BinanceMarketEvent ev{};
    ev.event_id = seq;
    ev.price_ticks = static_cast<std::int64_t>(seq * 3u + 1u);
    ev.qty_lots = static_cast<std::int64_t>(seq * 7u + 2u);
    ev.ts_event_ms = seq * 11u + 3u;
    ev.symbol_id = static_cast<std::uint32_t>(seq & 0xFFFFu);
    return ev;
}

bool item_is_intact(const BinanceMarketEvent& ev) {
    const std::uint64_t seq = ev.event_id;
    return ev.price_ticks == static_cast<std::int64_t>(seq * 3u + 1u)
        && ev.qty_lots == static_cast<std::int64_t>(seq * 7u + 2u)
        && ev.ts_event_ms == seq * 11u + 3u
        && ev.symbol_id == static_cast<std::uint32_t>(seq & 0xFFFFu);
}

// Per-thread results, collected without any gtest call on the worker thread.
struct ConsumerResult {
    std::uint64_t received{0};
    std::uint64_t first_out_of_order_at{UINT64_MAX};  // sentinel: none seen
    std::uint64_t out_of_order_count{0};
    std::uint64_t torn_payload_count{0};
    std::uint64_t first_torn_seq{UINT64_MAX};
};

struct ProducerResult {
    std::uint64_t pushed{0};
    std::uint64_t push_retries{0};  // times the ring was observed full
};

}  // namespace

// The core property: across a real thread boundary, every item the producer
// pushed is received exactly once, in the order it was pushed, byte-intact.
TEST(SpscRingConcurrency, NoLossNoDuplicationStrictFifo) {
    SpscRing<BinanceMarketEvent, kRingSlots> ring;
    ProducerResult prod{};
    ConsumerResult cons{};

    std::thread producer([&ring, &prod] {
        for (std::uint64_t i = 0; i < kItems; ++i) {
            const BinanceMarketEvent ev = make_item(i);
            while (!ring.try_push(ev)) {
                ++prod.push_retries;  // ring full — spin, never drop
            }
            ++prod.pushed;
        }
    });

    std::thread consumer([&ring, &cons] {
        BinanceMarketEvent out{};
        std::uint64_t expected = 0;
        while (cons.received < kItems) {
            if (!ring.try_pop(out)) {
                continue;  // ring empty — spin
            }
            if (out.event_id != expected) {
                ++cons.out_of_order_count;
                if (cons.first_out_of_order_at == UINT64_MAX) {
                    cons.first_out_of_order_at = expected;
                }
            }
            if (!item_is_intact(out)) {
                ++cons.torn_payload_count;
                if (cons.first_torn_seq == UINT64_MAX) {
                    cons.first_torn_seq = out.event_id;
                }
            }
            ++expected;
            ++cons.received;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(prod.pushed, kItems);
    EXPECT_EQ(cons.received, kItems) << "items lost or duplicated across the thread boundary";
    EXPECT_EQ(cons.out_of_order_count, 0u)
        << "FIFO order violated; first mismatch at expected seq " << cons.first_out_of_order_at;
    EXPECT_EQ(cons.torn_payload_count, 0u)
        << "torn/partially-visible slot observed; first at seq " << cons.first_torn_seq
        << " — this is what a missing release/acquire pair looks like on weak-memory hardware";

    // Not an assertion: on a fast machine the consumer may keep up and never
    // observe a full ring. Recorded as evidence so a run where the backpressure
    // path was never exercised is visible rather than assumed.
    RecordProperty("producer_push_retries", static_cast<int>(prod.push_retries));
}

// Same contract for the strategy->executor channel. IntentChannel is a second,
// independent SPSC boundary carrying ORDER INTENTS: an item dropped here is a
// silently unsent order, so it deserves its own coverage rather than inheriting
// confidence from the market-data ring.
TEST(IntentChannelConcurrency, NoLossNoDuplicationStrictFifo) {
    IntentChannel<kRingSlots> channel;
    constexpr std::uint64_t kIntents = 100'000;

    std::uint64_t pushed = 0;
    std::uint64_t received = 0;
    std::uint64_t order_violations = 0;
    std::uint64_t field_mismatches = 0;

    std::thread producer([&channel, &pushed] {
        for (std::uint64_t i = 0; i < kIntents; ++i) {
            TimestampedIntent ti{};
            ti.strategy_id = static_cast<std::uint32_t>(i & 0xFFFFFFFFu);
            ti.submit_ts_ns = i * 13u + 5u;
            while (!channel.try_push(ti)) {
                // full — spin, never drop an intent
            }
            ++pushed;
        }
    });

    std::thread consumer([&channel, &received, &order_violations, &field_mismatches] {
        TimestampedIntent out{};
        std::uint64_t expected = 0;
        while (received < kIntents) {
            if (!channel.try_pop(out)) {
                continue;
            }
            if (out.strategy_id != static_cast<std::uint32_t>(expected & 0xFFFFFFFFu)) {
                ++order_violations;
            }
            if (out.submit_ts_ns != expected * 13u + 5u) {
                ++field_mismatches;
            }
            ++expected;
            ++received;
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(pushed, kIntents);
    EXPECT_EQ(received, kIntents) << "an intent was lost — this would be a silently unsent order";
    EXPECT_EQ(order_violations, 0u);
    EXPECT_EQ(field_mismatches, 0u) << "torn TimestampedIntent observed across the boundary";
}

// size_approx() does two independent atomic loads and is documented as telemetry
// only. Two things are checked here:
//   * it never returns a value outside [0, capacity]. Two distinct failure
//     modes, both real, both found by this test rather than by inspection:
//     (a) reading head_ before tail_ let a concurrent pop advance tail_ past
//         the observed head_, underflowing the unsigned subtraction to
//         ~SIZE_MAX — fixed by reading tail_ first;
//     (b) even with tail_ read first (which rules out underflow), the gap
//         between the two loads has no upper bound, so head_ can advance by
//         more than one ring's worth of pushes before it's read — this
//         actually reproduced values above capacity() under real contention,
//         not just in theory — fixed by clamping the result to N.
//   * concurrent polling is atomic-vs-atomic, which is never a data race by
//     definition of the memory model, so TSan must stay silent on this loop.
TEST(SpscRingConcurrency, SizeApproxStaysInRangeUnderConcurrentPolling) {
    SpscRing<BinanceMarketEvent, kRingSlots> ring;
    constexpr std::uint64_t kOps = 100'000;
    std::atomic<bool> done{false};
    std::uint64_t out_of_range = 0;
    std::size_t max_seen = 0;

    std::thread producer([&ring, &done] {
        for (std::uint64_t i = 0; i < kOps; ++i) {
            const BinanceMarketEvent ev = make_item(i);
            while (!ring.try_push(ev)) {
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&ring, &done] {
        BinanceMarketEvent out{};
        std::uint64_t got = 0;
        while (got < kOps) {
            if (ring.try_pop(out)) {
                ++got;
            } else if (done.load(std::memory_order_acquire) && got >= kOps) {
                break;
            }
        }
    });

    // Poll from a third thread while the other two run.
    while (!done.load(std::memory_order_acquire)) {
        const std::size_t n = ring.size_approx();
        if (n > ring.capacity()) {
            ++out_of_range;
        } else if (n > max_seen) {
            max_seen = n;
        }
    }

    producer.join();
    consumer.join();

    EXPECT_EQ(out_of_range, 0u)
        << "size_approx() returned a value outside [0, capacity] — either underflowed (wrong "
           "load order) or exceeded capacity (unclamped over-report in the gap between the two "
           "independent loads)";
    RecordProperty("max_observed_occupancy", static_cast<int>(max_seen));
}
