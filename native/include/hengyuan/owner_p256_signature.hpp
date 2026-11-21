// SPDX-License-Identifier: proprietary
#pragma once

#include <openssl/evp.h>
#include <openssl/x509.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace hy::owner_signature {

inline constexpr std::size_t kMaxPublicKeyBytes = 1024;
inline constexpr std::size_t kMaxSignatureBytes = 72;
using PublicKey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

inline PublicKey decode_p256(std::span<const unsigned char> der) noexcept {
    if (der.empty() || der.size() > kMaxPublicKeyBytes) return PublicKey(nullptr, EVP_PKEY_free);
    const auto* p = der.data();
    PublicKey key(d2i_PUBKEY(nullptr, &p, static_cast<long>(der.size())), EVP_PKEY_free);
    if (!key || p != der.data() + der.size() || EVP_PKEY_is_a(key.get(), "EC") != 1) {
        return PublicKey(nullptr, EVP_PKEY_free);
    }
    std::array<char, 32> group{};
    if (EVP_PKEY_get_group_name(key.get(), group.data(), group.size(), nullptr) != 1 ||
        std::string_view(group.data()) != "prime256v1") return PublicKey(nullptr, EVP_PKEY_free);
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> check(
        EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr), EVP_PKEY_CTX_free);
    if (!check || EVP_PKEY_public_check(check.get()) != 1) return PublicKey(nullptr, EVP_PKEY_free);
    return key;
}

inline bool verify_p256(EVP_PKEY& key, std::span<const std::byte> payload,
                        std::span<const unsigned char> der_signature) noexcept {
    if (der_signature.empty() || der_signature.size() > kMaxSignatureBytes) return false;
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return ctx && EVP_DigestVerifyInit(ctx.get(), nullptr, EVP_sha256(), nullptr, &key) == 1 &&
           EVP_DigestVerify(ctx.get(), der_signature.data(), der_signature.size(),
                            reinterpret_cast<const unsigned char*>(payload.data()), payload.size()) == 1;
}

}  // namespace hy::owner_signature
