// SPDX-License-Identifier: proprietary
#pragma once

#include <hengyuan/owner_reset_approval.hpp>
#include <hengyuan/spot_owner_key_store.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hy {

class ProductionResetKey {
public:
    static std::string enrollment_path(const std::string& audit_path) {
        return audit_path + ".owner-reset.pub";
    }

    static ExportStatus enroll(const spot_owner_key_store::StoreFiles& files, std::uint64_t owner_key_id,
                               const std::string& public_key_der_path) {
        return spot_owner_key_store::enroll(files, StoreEnvironment::Production, owner_key_id, public_key_der_path,
                                            enrollment_path(files.log), kFormatTag);
    }

    static std::unique_ptr<OwnerResetAuthority> load(const std::string& audit_path, const DurableAuditSink& sink) {
        const auto record = spot_owner_key_store::read_enrollment(enrollment_path(audit_path), kFormatTag, sink);
        if (!record || record->binding.environment != StoreEnvironment::Production) return {};
        const OperatorResetScope scope{record->binding.environment, record->binding.account_uid,
                                        record->uuid_lo, record->uuid_hi};
        return OwnerResetAuthority::create_for_scope(spot_owner_key_store::unsigned_bytes(record->der), scope);
    }

private:
    static constexpr std::string_view kFormatTag = "HYRSTPK1";
};

}  // namespace hy
