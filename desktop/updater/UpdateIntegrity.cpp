#include "UpdateIntegrity.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

class BCryptAlgorithm final {
public:
    ~BCryptAlgorithm() {
        if (handle_ != nullptr) {
            BCryptCloseAlgorithmProvider(handle_, 0);
        }
    }
    BCRYPT_ALG_HANDLE* Put() { return &handle_; }
    BCRYPT_ALG_HANDLE Get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

class BCryptHashHandle final {
public:
    ~BCryptHashHandle() {
        if (handle_ != nullptr) {
            BCryptDestroyHash(handle_);
        }
    }
    BCRYPT_HASH_HANDLE* Put() { return &handle_; }
    BCRYPT_HASH_HANDLE Get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

std::wstring BCryptError(std::wstring_view operation, NTSTATUS status) {
    std::wostringstream message;
    message << operation << L" failed (NTSTATUS 0x" << std::hex
            << static_cast<unsigned long>(status) << L")";
    return message.str();
}

} // namespace

std::optional<std::wstring> Sha256File(const std::filesystem::path& path,
                                       std::wstring& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = L"Could not open file for SHA-256";
        return std::nullopt;
    }

    BCryptAlgorithm algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        algorithm.Put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        error = BCryptError(L"BCryptOpenAlgorithmProvider", status);
        return std::nullopt;
    }

    DWORD objectLength = 0;
    DWORD resultLength = 0;
    status = BCryptGetProperty(algorithm.Get(), BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &resultLength, 0);
    if (status < 0 || objectLength == 0) {
        error = BCryptError(L"BCryptGetProperty(BCRYPT_OBJECT_LENGTH)", status);
        return std::nullopt;
    }
    DWORD hashLength = 0;
    status = BCryptGetProperty(algorithm.Get(), BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&hashLength),
                              sizeof(hashLength), &resultLength, 0);
    if (status < 0 || hashLength != 32) {
        error = BCryptError(L"BCryptGetProperty(BCRYPT_HASH_LENGTH)", status);
        return std::nullopt;
    }

    std::vector<UCHAR> hashObject(objectLength);
    std::array<UCHAR, 32> digest{};
    BCryptHashHandle hash;
    status = BCryptCreateHash(algorithm.Get(), hash.Put(), hashObject.data(),
                              objectLength, nullptr, 0, 0);
    if (status < 0) {
        error = BCryptError(L"BCryptCreateHash", status);
        return std::nullopt;
    }

    std::vector<char> buffer(1024 * 1024);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0) {
            status = BCryptHashData(
                hash.Get(), reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(read), 0);
            if (status < 0) {
                error = BCryptError(L"BCryptHashData", status);
                return std::nullopt;
            }
        }
    }
    if (input.bad()) {
        error = L"Could not read file while computing SHA-256";
        return std::nullopt;
    }
    status = BCryptFinishHash(hash.Get(), digest.data(),
                              static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        error = BCryptError(L"BCryptFinishHash", status);
        return std::nullopt;
    }

    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool VerifyManagedFiles(const std::filesystem::path& root,
                        const UpdateArtifact& artifact,
                        std::wstring& error) {
    error.clear();
    if (artifact.files.empty()) {
        error = L"Artifact file manifest is empty";
        return false;
    }
    for (const auto& [relativePath, expectedDigest] : artifact.files) {
        if (!IsSafeUpdateRelativePath(relativePath) ||
            expectedDigest.size() != 64) {
            error = L"Artifact contains an invalid file path or digest";
            return false;
        }
        const auto path = root / std::filesystem::path(relativePath);
        std::error_code statusError;
        if (!std::filesystem::is_regular_file(path, statusError) || statusError) {
            error = L"Managed file is missing or is not a regular file";
            return false;
        }
        const auto actualDigest = Sha256File(path, error);
        if (!actualDigest || *actualDigest != expectedDigest) {
            if (error.empty()) {
                error = L"Managed file SHA-256 does not match the manifest";
            }
            return false;
        }
    }
    return true;
}
