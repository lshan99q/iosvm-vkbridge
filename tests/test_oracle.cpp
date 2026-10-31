// SPDX-License-Identifier: MIT

#include "test_framework.hpp"
#include "vkbridge/oracle.hpp"
#include "vkbridge/pattern.hpp"

#include <cstdint>

VKB_TEST(oracle_checksum_xors_32bit_words)
{
    uint8_t data[12] = {};
    vkbridge::write_le32(data + 0, 0x11223344u);
    vkbridge::write_le32(data + 4, 0x55667788u);
    vkbridge::write_le32(data + 8, 0x99AABBCCu);

    const uint32_t expected = 0x11223344u ^ 0x55667788u ^ 0x99AABBCCu;
    VKB_CHECK_EQ(vkbridge::attestation_checksum(data, sizeof(data), 0), expected);
    VKB_CHECK_EQ(vkbridge::attestation_checksum(data, sizeof(data), 0x00001234u),
                 expected ^ 0x00001234u);
}

VKB_TEST(oracle_checksum_handles_empty_range)
{
    VKB_CHECK_EQ(vkbridge::attestation_checksum(nullptr, 0, 0xABCDEF01u), 0xABCDEF01u);
}

VKB_TEST(oracle_fingerprint_is_marked)
{
    // The host expects the second attestation word to carry the marker XORed with the
    // nonce; the shader writes exactly that, so the values must agree bit for bit.
    VKB_CHECK_EQ(vkbridge::kAttestationFingerprint, 0x4F524143u);
    VKB_CHECK_EQ(vkbridge::kAttestationFingerprint ^ 0x12345678u, 0x5D66173Bu);
}
