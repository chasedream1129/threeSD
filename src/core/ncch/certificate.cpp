// Copyright 2021 Pengfei Zhu
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <unordered_map>
#include <cryptopp/sha.h>
#include "common/alignment.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/data_container.h"
#include "core/ncch/certificate.h"
#include "core/ncch/cia_common.h"

namespace Core {

// Sizes include padding (0x34 for RSA, 0x3C for ECC)
inline std::size_t GetPublicKeySize(u32 public_key_type) {
    switch (public_key_type) {
    case PublicKeyType::RSA_4096:
        return 0x238;
    case PublicKeyType::RSA_2048:
        return 0x138;
    case PublicKeyType::ECC:
        return 0x78;
    }

    LOG_ERROR(Common_Filesystem, "Tried to read cert with bad public key {}", public_key_type);
    return 0;
}

std::size_t Certificate::Load(std::vector<u8> file_data, std::size_t offset) {
    std::size_t total_size = static_cast<std::size_t>(file_data.size() - offset);
    if (total_size < sizeof(u32))
        return 0;

    std::memcpy(&signature_type, &file_data[offset], sizeof(u32));

    // Signature lengths are variable, and the body follows the signature
    u32 signature_size = GetSignatureSize(signature_type);
    if (signature_size == 0) {
        return 0;
    }

    // The certificate body start position is rounded to the nearest 0x40 after the signature
    std::size_t body_start = Common::AlignUp(signature_size + sizeof(u32), 0x40);
    std::size_t body_end = body_start + sizeof(Body);

    if (total_size < body_end)
        return 0;

    // Read signature + certificate body
    signature.resize(signature_size);
    std::memcpy(signature.data(), &file_data[offset + sizeof(u32)], signature_size);
    std::memcpy(&body, &file_data[offset + body_start], sizeof(Body));

    // Public key lengths are variable
    std::size_t public_key_size = GetPublicKeySize(body.key_type);
    if (public_key_size == 0) {
        return 0;
    }
    public_key.resize(public_key_size);
    std::memcpy(public_key.data(), &file_data[offset + body_end], public_key.size());

    return body_end + public_key.size();
}

bool Certificate::Save(FileUtil::IOFile& file) const {
    // signature
    if (file.WriteBytes(&signature_type, sizeof(signature_type)) != sizeof(signature_type) ||
        file.WriteBytes(signature.data(), signature.size()) != signature.size()) {

        LOG_ERROR(Core, "Failed to write signature");
        return false;
    }

    // body
    const std::size_t body_start = Common::AlignUp(signature.size() + sizeof(u32), 0x40);
    const std::size_t body_end = body_start + sizeof(body);
    if (!file.Seek(body_start - signature.size() - sizeof(u32), SEEK_CUR) ||
        file.WriteBytes(&body, sizeof(body)) != sizeof(body)) {

        LOG_ERROR(Core, "Failed to write body");
        return false;
    }

    // public key
    if (file.WriteBytes(public_key.data(), public_key.size()) != public_key.size()) {
        LOG_ERROR(Core, "Failed to write public key");
        return false;
    }

    return true;
}

namespace Certs {

static std::unordered_map<std::string, Certificate> g_certs;
static bool g_is_loaded = false;

bool Load(const std::string& path) {
    g_certs.clear();

    FileUtil::IOFile file(path, "rb");
    DataContainer container(file.GetData());
    std::vector<std::vector<u8>> data;
    if (!container.IsGood() || !container.GetIVFCLevel4Data(data)) {
        return false;
    }

    CertsDBHeader header;
    TRY_MEMCPY(&header, data[0], 0, sizeof(header));

    if (header.magic != MakeMagic('C', 'E', 'R', 'T')) {
        LOG_ERROR(Core, "File is invalid {}", path);
        return false;
    }

    const auto total_size = header.size + sizeof(header);
    if (data[0].size() < total_size) {
        LOG_ERROR(Core, "File {} header reports invalid size, may be corrupted", path);
        return false;
    }

    std::size_t pos = sizeof(header);
    while (pos < total_size) {
        Certificate cert;
        const auto size = cert.Load(data[0], pos);
        if (!size) { // Failed to load
            return false;
        }

        const auto name = Common::StringFromFixedZeroTerminatedBuffer(cert.body.name.data(),
                                                                      cert.body.name.size());
        g_certs.emplace(name, std::move(cert));

        pos += size;
    }

    for (const auto& cert : CIACertNames) {
        if (!g_certs.count(cert)) {
            LOG_ERROR(Core, "Cert {} required for CIA building but does not exist", cert);
            return false;
        }
    }

    g_is_loaded = true;
    return true;
}

bool IsLoaded() {
    return g_is_loaded;
}

const Certificate& Get(const std::string& name) {
    return g_certs.at(name);
}

} // namespace Certs

} // namespace Core
