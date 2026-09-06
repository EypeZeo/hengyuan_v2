// Batch 5, 5a: strategy_spec_toml_parser.cpp unit tests.
// Covers the happy path (docs/STRATEGY_SPEC.md §7's worked example, verbatim) and every
// fail-closed rejection path this loader is required to enforce.

#include <gtest/gtest.h>
#include <hengyuan/strategy_spec_toml_parser.hpp>

#include <string>

using namespace hy;

namespace {

// docs/STRATEGY_SPEC.md §7's worked example, character-for-character.
constexpr std::string_view kWorkedExample = R"(
spec_version = 1
name = "sma_crossover_btc_1h"
description = "20/50 SMA 金叉做多，用 RSI 过滤超买"

[market]
market    = "crypto_spot"
symbol    = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "sma"
input = "close"
window = 20

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[[indicators]]
id = "rsi14"
op = "rsi"
input = "close"
window = 14

[[indicators]]
id = "not_overbought"
op = "lt"
left = "rsi14"
right = 70.0

[[indicators]]
id = "trend_up"
op = "gt"
left = "fast"
right = "slow"

[[indicators]]
id = "entry"
op = "and"
left = "trend_up"
right = "not_overbought"

[signal]
node = "entry"
mode = "boolean"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "walk_forward+cpcv"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0000000000000000000000000000000000000000000000000000000000000000"
oos_sharpe     = 1.34
pbo            = 0.21
trials         = 480
)";

// Same shape, minus [validation] -- for tests that need a spec that otherwise loads fine.
std::string worked_example_without_validation() {
    const auto pos = std::string(kWorkedExample).find("\n[validation]");
    return std::string(kWorkedExample).substr(0, pos) + "\n";
}

}  // namespace

TEST(StrategySpecTomlParser, WorkedExampleLoadsSuccessfully) {
    const auto result = load_strategy_spec(kWorkedExample);
    ASSERT_EQ(result.status, SpecLoadStatus::Ok);
    EXPECT_TRUE(result.ok());

    ASSERT_EQ(result.dag.node_count, 6u);
    EXPECT_EQ(std::string_view(result.dag.nodes[0].id), "fast");
    EXPECT_EQ(result.dag.nodes[0].op, SpecOp::Sma);
    EXPECT_EQ(result.dag.nodes[0].param, 20);
    EXPECT_EQ(std::string_view(result.dag.nodes[5].id), "entry");
    EXPECT_EQ(result.dag.nodes[5].op, SpecOp::And);

    EXPECT_EQ(std::string_view(result.dag.nodes[result.dag.signal_node_idx].id), "entry");
    EXPECT_EQ(result.dag.signal_mode, SignalMode::Boolean);

    EXPECT_EQ(result.market.market, ManualMarket::CryptoSpot);
    EXPECT_EQ(std::string_view(result.market.symbol), "BTCUSDT");
    EXPECT_EQ(std::string_view(result.market.timeframe), "1h");

    EXPECT_TRUE(result.validation.present);
    EXPECT_EQ(std::string_view(result.validation.validated_at), "2026-09-15T08:30:00Z");
    EXPECT_DOUBLE_EQ(result.validation.oos_sharpe, 1.34);
    EXPECT_DOUBLE_EQ(result.validation.pbo, 0.21);
    EXPECT_EQ(result.validation.trials, 480);
}

TEST(StrategySpecTomlParser, NotOverboughtNodeCarriesLiteralRightOperand) {
    // "right = 70.0" on a comparison op (lt) -- a literal is legal here (lt is a binary value
    // op, not string-only for either operand).
    const auto result = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(result.ok());
    const auto& node = result.dag.nodes[3];  // not_overbought
    ASSERT_EQ(std::string_view(node.id), "not_overbought");
    EXPECT_EQ(node.op, SpecOp::Lt);
    EXPECT_EQ(node.operands[0].kind, OperandKind::NodeRef);  // left = rsi14
    EXPECT_EQ(node.operands[1].kind, OperandKind::Literal);  // right = 70.0
    EXPECT_DOUBLE_EQ(node.operands[1].literal_value, 70.0);
}

TEST(StrategySpecTomlParser, MissingSpecVersionRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("spec_version = 1\n");
    bad.erase(pos, std::string("spec_version = 1\n").size());
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::UnsupportedSpecVersion);
}

TEST(StrategySpecTomlParser, WrongSpecVersionRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("spec_version = 1\n");
    bad.replace(pos, std::string("spec_version = 1\n").size(), "spec_version = 2\n");
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::UnsupportedSpecVersion);
}

TEST(StrategySpecTomlParser, InvalidNameRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("name = \"sma_crossover_btc_1h\"\n");
    bad.replace(pos, std::string("name = \"sma_crossover_btc_1h\"\n").size(),
                "name = \"Has Spaces\"\n");
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::MissingRequiredField);
}

TEST(StrategySpecTomlParser, MissingMarketSymbolRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("symbol    = \"BTCUSDT\"\n");
    bad.erase(pos, std::string("symbol    = \"BTCUSDT\"\n").size());
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::MissingMarketBinding);
}

TEST(StrategySpecTomlParser, UnknownOperatorRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("op = \"sma\"\ninput = \"close\"\nwindow = 20");
    bad.replace(pos, std::string("op = \"sma\"").size(), "op = \"not_a_real_op\"");
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::UnknownOperator);
}

TEST(StrategySpecTomlParser, ForwardReferenceViolationRejected) {
    // "fast" node tries to reference "slow", which is declared LATER -- illegal per §2.2.
    constexpr std::string_view kSpec = R"(
spec_version = 1
name = "fwd_ref_violation"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "fast"
op = "gt"
left = "slow"
right = "close"

[[indicators]]
id = "slow"
op = "sma"
input = "close"
window = 50

[signal]
node = "fast"
mode = "boolean"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    EXPECT_EQ(load_strategy_spec(kSpec).status, SpecLoadStatus::ForwardReferenceViolation);
}

TEST(StrategySpecTomlParser, DuplicateNodeIdRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("id = \"slow\"\n");
    bad.replace(pos, std::string("id = \"slow\"\n").size(), "id = \"fast\"\n");
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::DuplicateNodeId);
}

TEST(StrategySpecTomlParser, MissingValidationSectionRejected) {
    // C++ loader is strictly more restrictive than schema.py here -- deliberate divergence,
    // see strategy_spec_toml_parser.hpp's own header comment.
    EXPECT_EQ(load_strategy_spec(worked_example_without_validation()).status,
              SpecLoadStatus::MissingValidationSection);
}

TEST(StrategySpecTomlParser, IncompleteValidationSectionRejected) {
    std::string bad = std::string(kWorkedExample);
    const auto pos = bad.find("trials         = 480\n");
    bad.erase(pos, std::string("trials         = 480\n").size());
    EXPECT_EQ(load_strategy_spec(bad).status, SpecLoadStatus::IncompleteValidationSection);
}

TEST(StrategySpecTomlParser, TooManyIndicatorNodesRejected) {
    std::string spec = R"(
spec_version = 1
name = "too_many_nodes"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

)";
    // kMaxIndicatorNodes(64) + 1 trivial nodes, each referencing "close" directly (no
    // inter-node references needed to trigger the capacity check).
    for (int i = 0; i < 65; ++i) {
        spec += "[[indicators]]\nid = \"n" + std::to_string(i) +
                "\"\nop = \"sma\"\ninput = \"close\"\nwindow = 5\n\n";
    }
    spec += "[signal]\nnode = \"n0\"\nmode = \"boolean\"\n";
    EXPECT_EQ(load_strategy_spec(spec).status, SpecLoadStatus::TooManyIndicatorNodes);
}

TEST(StrategySpecTomlParser, StddevWindowBelowTwoRejected) {
    constexpr std::string_view kSpec = R"(
spec_version = 1
name = "bad_stddev_window"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "vol"
op = "stddev"
input = "close"
window = 1

[signal]
node = "vol"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    EXPECT_EQ(load_strategy_spec(kSpec).status, SpecLoadStatus::InvalidOperatorParameter);
}

TEST(StrategySpecTomlParser, LiteralForbiddenOnWindowedOpInputRejected) {
    // "input" must reference a raw field or an earlier node id -- a bare literal there is
    // illegal (schema.py's _string_only_operand_fields()).
    constexpr std::string_view kSpec = R"(
spec_version = 1
name = "literal_input_rejected"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "bad"
op = "sma"
input = 5.0
window = 10

[signal]
node = "bad"
mode = "scaled"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    EXPECT_EQ(load_strategy_spec(kSpec).status, SpecLoadStatus::InvalidOperatorParameter);
}

TEST(StrategySpecTomlParser, CrossesAboveAcceptsLiteralOperand) {
    // Verified spec<->implementation arbitration (Batch 5, 5a): crosses_above/crosses_below
    // accept a literal operand despite operators.py's docstring implying "both need a genuine
    // Series" -- evaluator.py's _resolve_series() actually broadcasts a bare literal into a
    // constant series, and schema.py never restricts these two ops' operands to string-only.
    constexpr std::string_view kSpec = R"(
spec_version = 1
name = "crosses_literal_ok"

[market]
market = "crypto_spot"
symbol = "BTCUSDT"
timeframe = "1h"

[[indicators]]
id = "broke_100"
op = "crosses_above"
left = "close"
right = 100.0

[signal]
node = "broke_100"
mode = "boolean"

[validation]
validated_at   = "2026-09-15T08:30:00Z"
validated_by   = "x"
data_start_utc = "2021-01-01T00:00:00Z"
data_end_utc   = "2026-06-30T00:00:00Z"
data_digest    = "sha256:0"
oos_sharpe     = 1.0
pbo            = 0.1
trials         = 1
)";
    const auto result = load_strategy_spec(kSpec);
    ASSERT_TRUE(result.ok());
    const auto& node = result.dag.nodes[0];
    EXPECT_EQ(node.op, SpecOp::CrossesAbove);
    EXPECT_EQ(node.operands[0].kind, OperandKind::RawFieldRef);
    EXPECT_EQ(node.operands[1].kind, OperandKind::Literal);
    EXPECT_DOUBLE_EQ(node.operands[1].literal_value, 100.0);
}

TEST(StrategySpecTomlParser, MarketBindingMatchesChecksAllThreeFields) {
    const auto result = load_strategy_spec(kWorkedExample);
    ASSERT_TRUE(result.ok());
    EXPECT_TRUE(market_binding_matches(result.market, ManualMarket::CryptoSpot, "BTCUSDT", "1h"));
    EXPECT_FALSE(market_binding_matches(result.market, ManualMarket::CryptoSpot, "ETHUSDT", "1h"));
    EXPECT_FALSE(market_binding_matches(result.market, ManualMarket::CryptoSpot, "BTCUSDT", "15m"));
    EXPECT_FALSE(market_binding_matches(result.market, ManualMarket::AShare, "BTCUSDT", "1h"));
}
