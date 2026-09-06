// Batch 5, 5c: the cross-language consistency test docs/STRATEGY_SPEC.md §2.2/§6/§8 requires
// ("同一份 spec + 同一段历史数据，两侧逐 bar 输出必须相同"). Zero Python dependency -- reads a
// checked-in, offline-generated golden fixture (native/tests/fixtures/strategy_spec/*.golden.json,
// produced by py_core/tools/generate_strategy_spec_fixture.py) via simdjson's DOM API (this is
// a one-shot, load-once-then-read-fully cold path, unlike binance_json_parser.hpp's per-message
// hot-path ondemand API -- DOM is the simpler, equally-available fit here), steps a
// StreamingEvaluator bar-by-bar, and asserts every node's per-bar value matches exactly.
//
// Fixtures are machine-generated only -- never hand-edited. If this test fails after someone
// changes an operator in py_core/indicators/operators.py or its C++ port in
// strategy_spec_operators.hpp, the fix is almost always "regenerate the fixture" (if the
// change was intentional and both sides still agree) or "the C++/Python ports have drifted"
// (if only one side was changed) -- see this repo's Batch 5 plan for the full rationale.

#include <gtest/gtest.h>
#include <hengyuan/strategy_spec_evaluator.hpp>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <simdjson.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace hy;

namespace {

struct Fixture {
    std::vector<Bar> bars;
    std::unordered_map<std::string, std::vector<double>> node_outputs;
    std::uint32_t effective_warmup{0};
};

std::string fixture_path(const char* filename) {
    return std::string(HY_STRATEGY_SPEC_FIXTURE_DIR) + "/" + filename;
}

std::string read_whole_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// double NaN <-> JSON null, per this fixture format's own documented convention (see
// py_core/tools/generate_strategy_spec_fixture.py's header comment) -- JSON has no native NaN
// token.
std::optional<Fixture> load_fixture(const std::string& json_path) {
    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.load(json_path).get(doc)) return std::nullopt;

    Fixture fixture;

    simdjson::dom::array bars_arr;
    if (doc["bars"].get_array().get(bars_arr)) return std::nullopt;
    for (simdjson::dom::element bar_elem : bars_arr) {
        Bar bar{};
        double v{};
        if (bar_elem["open"].get_double().get(v)) return std::nullopt;
        bar.open = v;
        if (bar_elem["high"].get_double().get(v)) return std::nullopt;
        bar.high = v;
        if (bar_elem["low"].get_double().get(v)) return std::nullopt;
        bar.low = v;
        if (bar_elem["close"].get_double().get(v)) return std::nullopt;
        bar.close = v;
        if (bar_elem["volume"].get_double().get(v)) return std::nullopt;
        bar.volume = v;
        fixture.bars.push_back(bar);
    }

    simdjson::dom::object node_outputs_obj;
    if (doc["node_outputs"].get_object().get(node_outputs_obj)) return std::nullopt;
    for (auto [key, value] : node_outputs_obj) {
        simdjson::dom::array series_arr;
        if (value.get_array().get(series_arr)) return std::nullopt;
        std::vector<double> series;
        series.reserve(fixture.bars.size());
        for (simdjson::dom::element elem : series_arr) {
            if (elem.is_null()) {
                series.push_back(std::numeric_limits<double>::quiet_NaN());
                continue;
            }
            double v{};
            if (elem.get_double().get(v)) return std::nullopt;
            series.push_back(v);
        }
        fixture.node_outputs.emplace(std::string(key), std::move(series));
    }

    int64_t warmup{};
    if (doc["effective_warmup"].get_int64().get(warmup)) return std::nullopt;
    fixture.effective_warmup = static_cast<std::uint32_t>(warmup);

    return fixture;
}

// Both-NaN counts as a pass (IEEE754: NaN != NaN, so a bare ASSERT_DOUBLE_EQ would misreport
// this exact-match case as a failure).
void assert_series_equal(double actual, double expected, std::size_t node_idx, std::size_t bar_idx) {
    if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(actual)) << "node " << node_idx << " bar " << bar_idx
                                         << ": expected NaN, got " << actual;
    } else {
        EXPECT_DOUBLE_EQ(actual, expected) << "node " << node_idx << " bar " << bar_idx;
    }
}

void run_consistency_case(const char* spec_filename, const char* fixture_filename) {
    const std::string toml_text = read_whole_file(fixture_path(spec_filename));
    ASSERT_FALSE(toml_text.empty()) << "failed to read " << spec_filename;
    const auto load = load_strategy_spec(toml_text);
    ASSERT_TRUE(load.ok()) << "failed to load " << spec_filename
                            << " (status=" << static_cast<int>(load.status) << ")";

    const auto fixture = load_fixture(fixture_path(fixture_filename));
    ASSERT_TRUE(fixture.has_value()) << "failed to load " << fixture_filename;

    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));
    EXPECT_EQ(evaluator.effective_warmup(), fixture->effective_warmup);

    for (std::size_t bar_idx = 0; bar_idx < fixture->bars.size(); ++bar_idx) {
        evaluator.step(fixture->bars[bar_idx]);
        for (std::size_t node_idx = 0; node_idx < load.dag.node_count; ++node_idx) {
            const std::string node_id(load.dag.nodes[node_idx].id);
            const auto it = fixture->node_outputs.find(node_id);
            ASSERT_NE(it, fixture->node_outputs.end())
                << "fixture missing node_outputs for " << node_id;
            ASSERT_LT(bar_idx, it->second.size());
            assert_series_equal(evaluator.node_value(node_idx), it->second[bar_idx], node_idx, bar_idx);
        }
    }
}

}  // namespace

TEST(StrategySpecConsistency, WorkedExampleMatchesPythonBarForBar) {
    run_consistency_case("sma_crossover_btc_1h.spec.toml", "sma_crossover_btc_1h.spec.golden.json");
}

// Explicit warm-up-boundary check (§205's own requirement: "批次 5 的一致性测试必须覆盖
// warm-up 边界那几根 bar") -- folded into the loop above via assert_series_equal on every bar
// including the boundary, but this test isolates just that region so a boundary-specific
// failure is never lost inside a 120-bar loop's aggregate output.
TEST(StrategySpecConsistency, WorkedExampleWarmupBoundaryBarsMatchExactly) {
    const std::string toml_text = read_whole_file(fixture_path("sma_crossover_btc_1h.spec.toml"));
    ASSERT_FALSE(toml_text.empty());
    const auto load = load_strategy_spec(toml_text);
    ASSERT_TRUE(load.ok());

    const auto fixture = load_fixture(fixture_path("sma_crossover_btc_1h.spec.golden.json"));
    ASSERT_TRUE(fixture.has_value());
    ASSERT_EQ(fixture->effective_warmup, 50u);

    StreamingEvaluator evaluator;
    ASSERT_TRUE(evaluator.init(load.dag));

    // "entry" is nodes[5] per the declared order, matching the other StrategySpec test files'
    // own confirmed node ordering for this exact fixture text.
    constexpr std::size_t kEntryIdx = 5;
    const auto& entry_expected = fixture->node_outputs.at("entry");

    // Bars 47..52 (0-indexed) straddle the warm-up boundary at 49/50 -- the doc's own
    // "必须覆盖 warm-up 边界那几根 bar" requirement, isolated from the full-series loop above.
    for (std::size_t bar_idx = 0; bar_idx < fixture->bars.size(); ++bar_idx) {
        evaluator.step(fixture->bars[bar_idx]);
        if (bar_idx < 47 || bar_idx > 52) continue;
        assert_series_equal(evaluator.node_value(kEntryIdx), entry_expected[bar_idx], kEntryIdx, bar_idx);
    }
}

TEST(StrategySpecConsistency, OperatorCoverageMatchesPythonBarForBar) {
    run_consistency_case("operator_coverage.spec.toml", "operator_coverage.spec.golden.json");
}
