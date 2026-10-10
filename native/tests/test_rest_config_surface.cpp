// SPDX-License-Identifier: proprietary
// test_rest_config_surface.cpp -- fault case FI-032 (audit P1-001) and the compile-time half of the
// SAFE-01 freeze guard.
//
// FI-032: "a production build is handed an injectable host or a self-signed CA". The required outcome is
// that the production types do not expose such a field at all -- only the test-seam type can carry one.
// These tests state that as properties of the types, so re-adding either field to ANY of the three
// config structs, making the seam fillable from production code, or letting a network coroutine take a
// plain host again fails here. The scan of the source tree for the same things (and for new connection
// sites) is tests/test_rest_freeze_guard.cpp; this file needs Boost because it names the real types.

#include <gtest/gtest.h>
#include <hengyuan/binance_klines_rest.hpp>
#include <hengyuan/binance_private_rest.hpp>
#include <hengyuan/binance_rest_snapshot.hpp>
#include <hengyuan/rest_test_seam.hpp>

#include "test_helpers/rest_test_seam_builder.hpp"

#include <string>
#include <string_view>
#include <type_traits>

using hy::EndpointPermit;
using hy::PrivateRestConfig;
using hy::PublicRestConfig;
using hy::RestSnapshotConfig;
using hy::RestTestSeam;
using hy::RestTestSeamBuilder;

namespace {

// Dependent on T so that an absent (or inaccessible) member makes the requirement false instead of a
// hard error.
template <class T>
constexpr bool has_connect_host_override = requires(T t) { t.connect_host_override; };
template <class T>
constexpr bool has_extra_trusted_ca_pem_path = requires(T t) { t.extra_trusted_ca_pem_path; };

template <class T>
constexpr bool can_write_ca_field = requires(T s) { s.ca_pem_path_ = "x"; };
template <class T>
constexpr bool can_write_connect_field = requires(T s) { s.connect_host_ = "x"; };
template <class T>
constexpr bool can_write_through_ca_accessor = requires(T s) { s.extra_trusted_ca_pem_path() = "x"; };
template <class T>
constexpr bool can_write_through_connect_accessor = requires(T s) { s.connect_host_override() = "x"; };

// What a config looked like before the fix: the positive control for the two concepts above.
struct LegacyConfigShape {
    std::string port = "443";
    std::string extra_trusted_ca_pem_path;
    std::string connect_host_override;
};

template <class... A>
constexpr bool server_time_coro_callable = requires(A... a) { hy::detail::fetch_server_time_coro(a...); };
template <class... A>
constexpr bool signed_body_coro_callable = requires(A... a) { hy::detail::fetch_signed_body_coro(a...); };
template <class... A>
constexpr bool public_body_coro_callable = requires(A... a) { hy::detail::fetch_public_body_coro(a...); };

}  // namespace

// --- FI-032: no production config can carry the two abilities -------------------------------------------

TEST(RestConfigSurface, ThePositiveControlSeesTheOldShape) {
    EXPECT_TRUE(has_connect_host_override<LegacyConfigShape>);
    EXPECT_TRUE(has_extra_trusted_ca_pem_path<LegacyConfigShape>);
}

TEST(RestConfigSurface, NoRestConfigTypeCanNameAConnectTargetOverride) {
    EXPECT_FALSE(has_connect_host_override<PrivateRestConfig>);
    EXPECT_FALSE(has_connect_host_override<RestSnapshotConfig>);
    EXPECT_FALSE(has_connect_host_override<PublicRestConfig>);
}

TEST(RestConfigSurface, NoRestConfigTypeCanNameAnExtraTrustAnchor) {
    EXPECT_FALSE(has_extra_trusted_ca_pem_path<PrivateRestConfig>);
    EXPECT_FALSE(has_extra_trusted_ca_pem_path<RestSnapshotConfig>);
    EXPECT_FALSE(has_extra_trusted_ca_pem_path<PublicRestConfig>);
}

// --- the seam is the only carrier, and production code cannot fill it ---------------------------------

TEST(RestTestSeamSurface, TheEmptySeamIsWhatProductionPasses) {
    const RestTestSeam seam;
    EXPECT_TRUE(seam.extra_trusted_ca_pem_path().empty());
    EXPECT_TRUE(seam.connect_host_override().empty());
}

TEST(RestTestSeamSurface, OnlyTheBuilderCanFillIt) {
    EXPECT_TRUE(std::is_default_constructible_v<RestTestSeam>);
    EXPECT_FALSE((std::is_constructible_v<RestTestSeam, std::string, std::string>));
    EXPECT_FALSE(std::is_aggregate_v<RestTestSeam>) << "a brace-initialiser would fill it from anywhere";
    EXPECT_FALSE(can_write_ca_field<RestTestSeam>);
    EXPECT_FALSE(can_write_connect_field<RestTestSeam>);
    EXPECT_FALSE(can_write_through_ca_accessor<RestTestSeam>);
    EXPECT_FALSE(can_write_through_connect_accessor<RestTestSeam>);
}

TEST(RestTestSeamSurface, TheBuilderStillWorksSoTheChecksAboveAreNotVacuous) {
    const RestTestSeam loop = RestTestSeamBuilder::loopback("ca.pem");
    EXPECT_EQ(loop.extra_trusted_ca_pem_path(), "ca.pem");
    EXPECT_EQ(loop.connect_host_override(), "127.0.0.1");
    const RestTestSeam plain = RestTestSeamBuilder::loopback();
    EXPECT_TRUE(plain.extra_trusted_ca_pem_path().empty());
    EXPECT_EQ(plain.connect_host_override(), "127.0.0.1");
    const RestTestSeam custom = RestTestSeamBuilder::with("c.pem", "fixture.invalid");
    EXPECT_EQ(custom.extra_trusted_ca_pem_path(), "c.pem");
    EXPECT_EQ(custom.connect_host_override(), "fixture.invalid");
}

// --- freeze guard, compile-time half: the private client's coroutines take a permit, not a host ------

TEST(RestFreezeGuardTypes, ThePermitCannotBeMadeOutOfAHost) {
    EXPECT_FALSE(std::is_default_constructible_v<EndpointPermit>);
    EXPECT_FALSE((std::is_constructible_v<EndpointPermit, std::string_view>));
    EXPECT_FALSE((std::is_constructible_v<EndpointPermit, const char*>));
    EXPECT_FALSE((std::is_convertible_v<std::string_view, EndpointPermit>));
}

TEST(RestFreezeGuardTypes, TheServerTimeCoroutineTakesAPermitAndNoHostString) {
    EXPECT_TRUE((server_time_coro_callable<EndpointPermit, PrivateRestConfig, RestTestSeam>));
    EXPECT_FALSE((server_time_coro_callable<std::string, PrivateRestConfig, RestTestSeam>));
    EXPECT_FALSE((server_time_coro_callable<std::string_view, PrivateRestConfig, RestTestSeam>));
    EXPECT_FALSE((server_time_coro_callable<std::string, PrivateRestConfig>)) << "the pre-fix shape";
}

TEST(RestFreezeGuardTypes, TheSignedBodyCoroutineTakesAPermitAndNoHostString) {
    EXPECT_TRUE((signed_body_coro_callable<EndpointPermit, std::string, std::string, PrivateRestConfig,
                                           RestTestSeam>));
    EXPECT_FALSE((signed_body_coro_callable<std::string, std::string, std::string, PrivateRestConfig,
                                            RestTestSeam>));
    EXPECT_FALSE((signed_body_coro_callable<std::string, std::string, std::string, PrivateRestConfig>))
        << "the pre-fix shape";
}

TEST(RestFreezeGuardTypes, ThePublicBodyCoroutineTakesAPermitAndNoHostString) {
    EXPECT_TRUE((public_body_coro_callable<EndpointPermit, std::string, PrivateRestConfig, RestTestSeam>));
    EXPECT_FALSE((public_body_coro_callable<std::string, std::string, PrivateRestConfig, RestTestSeam>));
    EXPECT_FALSE((public_body_coro_callable<std::string, std::string, PrivateRestConfig>))
        << "the pre-fix shape";
}
