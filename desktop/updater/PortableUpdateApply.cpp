#include "PortableUpdateApply.h"

#include "UpdateIntegrity.h"
#include "UpdateManifest.h"

#include "miniz.h"

#include <sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {

constexpr std::uint64_t kMaximumExpandedBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaximumArchiveEntries = 2048;
constexpr auto kServiceStartTimeout = std::chrono::seconds(30);

class WinHandle final {
public:
    explicit WinHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~WinHandle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
    }
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE* put() { return &value_; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_;
};

struct OrdinalIgnoreCaseLess {
    bool operator()(std::wstring_view left, std::wstring_view right) const {
        return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                    right.data(), static_cast<int>(right.size()),
                                    TRUE) == CSTR_LESS_THAN;
    }
};

struct ZipReaderContext {
    HANDLE file = INVALID_HANDLE_VALUE;
};

size_t ReadArchiveAt(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
    auto* context = static_cast<ZipReaderContext*>(opaque);
    if (context == nullptr || context->file == INVALID_HANDLE_VALUE ||
        size > std::numeric_limits<DWORD>::max()) return 0;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(context->file, position, nullptr, FILE_BEGIN)) return 0;
    DWORD read = 0;
    if (!ReadFile(context->file, buffer, static_cast<DWORD>(size), &read, nullptr)) return 0;
    return read;
}

struct OutputContext {
    HANDLE file = INVALID_HANDLE_VALUE;
};

size_t WriteExtractedFile(void* opaque, mz_uint64 offset,
                          const void* buffer, size_t size) {
    auto* context = static_cast<OutputContext*>(opaque);
    if (context == nullptr || context->file == INVALID_HANDLE_VALUE ||
        size > std::numeric_limits<DWORD>::max()) return 0;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(context->file, position, nullptr, FILE_BEGIN)) return 0;
    DWORD written = 0;
    if (!WriteFile(context->file, buffer, static_cast<DWORD>(size), &written,
                   nullptr)) return 0;
    return written;
}

bool DecodeUtf8Path(std::string_view value, std::wstring& path) {
    path.clear();
    if (value.empty() || value.size() > 260) return false;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0 || count > 260) return false;
    path.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), path.data(), count) == count;
}

bool IsReparseOrSymlink(const mz_zip_archive_file_stat& entry) {
    constexpr mz_uint32 kWindowsReparseAttribute = 0x00000400;
    constexpr mz_uint32 kUnixFileTypeMask = 0170000;
    constexpr mz_uint32 kUnixSymlink = 0120000;
    const auto unixType = (entry.m_external_attr >> 16) & kUnixFileTypeMask;
    return (entry.m_external_attr & kWindowsReparseAttribute) != 0 ||
           unixType == kUnixSymlink;
}

bool IsDirectoryTreeSafe(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::recursive_directory_iterator current(
        root, std::filesystem::directory_options::none, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) return false;
    for (; current != end; current.increment(error)) {
        if (error) return false;
        const DWORD attributes = GetFileAttributesW(current->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
    }
    return true;
}

bool HasOnlyPortableRootEntries(const std::filesystem::path& root) {
    std::set<std::wstring, OrdinalIgnoreCaseLess> names;
    std::error_code error;
    for (std::filesystem::directory_iterator current(root, error), end;
         current != end && !error; current.increment(error)) {
        names.insert(current->path().filename().wstring());
    }
    if (error || names.size() != 2 ||
        names.find(L"CampusNetLauncher.exe") == names.end() ||
        names.find(L"bin") == names.end()) return false;
    const DWORD launcherAttributes =
        GetFileAttributesW((root / L"CampusNetLauncher.exe").c_str());
    const DWORD binAttributes = GetFileAttributesW((root / L"bin").c_str());
    return launcherAttributes != INVALID_FILE_ATTRIBUTES &&
           (launcherAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                  FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
           binAttributes != INVALID_FILE_ATTRIBUTES &&
           (binAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (binAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
           IsDirectoryTreeSafe(root);
}

bool HasRequiredFiles(const UpdateArtifact& artifact, std::wstring& error) {
    if (artifact.files.empty() || artifact.files.size() > 1024) {
        error = L"Portable file manifest is empty or exceeds its limit";
        return false;
    }
    std::set<std::wstring, OrdinalIgnoreCaseLess> paths;
    for (const auto& [path, digest] : artifact.files) {
        if (!IsSafeUpdateRelativePath(path) || digest.size() != 64 ||
            !std::all_of(digest.begin(), digest.end(), [](wchar_t ch) {
                return (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f');
            })) {
            error = L"Portable file manifest contains an unsafe path or digest";
            return false;
        }
        paths.insert(path);
        if (path != L"CampusNetLauncher.exe" && !path.starts_with(L"bin/")) {
            error = L"Portable package root may contain only its launcher and bin directory";
            return false;
        }
    }
    for (const auto required : {L"CampusNetLauncher.exe", L"bin/CampusNetClient.exe",
                                L"bin/CampusNetService.exe", L"bin/CampusNetUpdater.exe",
                                L"bin/version.json"}) {
        if (paths.find(required) == paths.end()) {
            error = L"Portable package is missing a required application file";
            return false;
        }
    }
    return true;
}

std::set<std::wstring, OrdinalIgnoreCaseLess> ExpectedDirectories(
    const UpdateArtifact& artifact) {
    std::set<std::wstring, OrdinalIgnoreCaseLess> directories;
    for (const auto& [path, digest] : artifact.files) {
        (void)digest;
        std::size_t slash = path.find(L'/');
        while (slash != std::wstring::npos) {
            directories.insert(path.substr(0, slash));
            slash = path.find(L'/', slash + 1);
        }
    }
    return directories;
}

bool EnsureSafeDirectory(const std::filesystem::path& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool CreateRestrictedDirectory(const std::filesystem::path& path) {
    WinHandle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) return false;
    DWORD tokenBytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &tokenBytes);
    if (tokenBytes == 0) return false;
    std::vector<std::byte> tokenBuffer(tokenBytes);
    if (!GetTokenInformation(token.get(), TokenUser, tokenBuffer.data(),
                             tokenBytes, &tokenBytes)) return false;
    auto* user = reinterpret_cast<TOKEN_USER*>(tokenBuffer.data());
    LPWSTR userSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &userSid)) return false;
    const std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" +
                              std::wstring(userSid) + L")";
    LocalFree(userSid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1,
            &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    const bool created = CreateDirectoryW(path.c_str(), &security) != FALSE;
    LocalFree(descriptor);
    if (!created) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool MakeUniqueSibling(const std::filesystem::path& parent,
                       std::wstring_view prefix,
                       std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return false;
        wchar_t text[40]{};
        if (StringFromGUID2(id, text, ARRAYSIZE(text)) == 0) return false;
        std::wstring compact;
        for (const wchar_t ch : std::wstring_view(text)) {
            if (ch != L'{' && ch != L'}' && ch != L'-' && ch != L'\0') {
                compact.push_back(ch);
            }
        }
        path = parent / (std::wstring(prefix) + compact);
        if (CreateRestrictedDirectory(path)) return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

bool UniqueSiblingName(const std::filesystem::path& parent,
                       std::wstring_view prefix,
                       std::filesystem::path& path) {
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return false;
        wchar_t text[40]{};
        if (StringFromGUID2(id, text, ARRAYSIZE(text)) == 0) return false;
        std::wstring compact;
        for (const wchar_t ch : std::wstring_view(text)) {
            if (ch != L'{' && ch != L'}' && ch != L'-' && ch != L'\0') {
                compact.push_back(ch);
            }
        }
        path = parent / (std::wstring(prefix) + compact);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    }
    return false;
}

bool ValidateVersionRecord(const std::filesystem::path& root,
                           std::wstring_view targetVersion,
                           std::wstring& error) {
    std::ifstream input(root / L"bin" / L"version.json", std::ios::binary);
    if (!input) {
        error = L"Updated package has no readable version.json";
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length <= 0 || length > 64 * 1024) {
        error = L"Updated package version.json has an invalid size";
        return false;
    }
    input.seekg(0, std::ios::beg);
    std::string json(static_cast<std::size_t>(length), '\0');
    if (!input.read(json.data(), static_cast<std::streamsize>(json.size()))) {
        error = L"Updated package version.json could not be read";
        return false;
    }
    LocalInstallInfo info;
    if (!ParseLocalInstallInfo(json, info, error) ||
        info.channel != UpdateChannel::Portable || info.version != targetVersion) {
        error = L"Updated package version/channel marker does not match the update";
        return false;
    }
    return true;
}

bool ReadZipPath(const mz_zip_archive_file_stat& entry, std::wstring& path,
                 bool& directory) {
    const std::size_t length = strnlen_s(entry.m_filename,
                                         sizeof(entry.m_filename));
    if (length == 0 || length >= sizeof(entry.m_filename)) return false;
    std::string_view name(entry.m_filename, length);
    directory = entry.m_is_directory != 0;
    if (directory) {
        if (name.back() != '/') return false;
        name.remove_suffix(1);
    } else if (name.back() == '/') {
        return false;
    }
    if (!DecodeUtf8Path(name, path) || !IsSafeUpdateRelativePath(path)) return false;
    return true;
}

bool ExtractValidatedArchive(const std::filesystem::path& archivePath,
                             const std::filesystem::path& destination,
                             std::wstring_view targetVersion,
                             const UpdateArtifact& artifact,
                             std::wstring& error) {
    WinHandle file(CreateFileW(archivePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        error = L"Verified update archive could not be opened";
        return false;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.get(), &length) || length.QuadPart <= 0) {
        error = L"Update archive has an invalid size";
        return false;
    }
    ZipReaderContext context{file.get()};
    mz_zip_archive zip{};
    zip.m_pRead = &ReadArchiveAt;
    zip.m_pIO_opaque = &context;
    if (!mz_zip_reader_init(&zip, static_cast<mz_uint64>(length.QuadPart), 0)) {
        error = L"Update archive is not a valid ZIP";
        return false;
    }
    struct ZipGuard {
        mz_zip_archive* archive;
        ~ZipGuard() { mz_zip_reader_end(archive); }
    } zipGuard{&zip};

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count == 0 || count > kMaximumArchiveEntries ||
        count > artifact.files.size() + 1024) {
        error = L"Update archive entry count is invalid";
        return false;
    }
    const auto expectedDirectories = ExpectedDirectories(artifact);
    std::set<std::wstring, OrdinalIgnoreCaseLess> seen;
    std::set<std::wstring, OrdinalIgnoreCaseLess> seenFiles;
    std::set<std::wstring, OrdinalIgnoreCaseLess> seenDirectories;
    std::uint64_t expandedTotal = 0;
    std::vector<mz_zip_archive_file_stat> entries;
    entries.reserve(count);
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat entry{};
        std::wstring path;
        bool directory = false;
        if (!mz_zip_reader_file_stat(&zip, index, &entry) ||
            !entry.m_is_supported || entry.m_is_encrypted ||
            IsReparseOrSymlink(entry) || !ReadZipPath(entry, path, directory) ||
            !seen.insert(path).second) {
            error = L"Update archive contains an invalid, duplicate, encrypted, or reparse entry";
            return false;
        }
        if (directory) {
            if (entry.m_uncomp_size != 0 ||
                expectedDirectories.find(path) == expectedDirectories.end() ||
                !seenDirectories.insert(path).second) {
                error = L"Update archive contains an unexpected directory entry";
                return false;
            }
        } else {
            const auto file = artifact.files.find(path);
            if (file == artifact.files.end() ||
                entry.m_uncomp_size > kMaximumExpandedBytes - expandedTotal) {
                error = L"Update archive contains an unmanifested file or exceeds the expansion limit";
                return false;
            }
            if (entry.m_method != 0 && entry.m_method != 8) {
                error = L"Update archive uses an unsupported compression method";
                return false;
            }
            expandedTotal += entry.m_uncomp_size;
            seenFiles.insert(path);
        }
        entries.push_back(entry);
    }
    if (seenFiles.size() != artifact.files.size() ||
        !mz_zip_validate_archive(&zip, 0)) {
        error = L"Update archive does not exactly match the file manifest or has invalid contents";
        return false;
    }

    for (const auto& entry : entries) {
        std::wstring path;
        bool directory = false;
        if (!ReadZipPath(entry, path, directory)) {
            error = L"Update archive path became invalid during extraction";
            return false;
        }
        const auto destinationPath = destination /
            std::filesystem::path(path).wstring();
        if (directory) {
            if (!EnsureSafeDirectory(destinationPath)) {
                error = L"Could not create an update package directory safely";
                return false;
            }
            continue;
        }
        if (!EnsureSafeDirectory(destinationPath.parent_path())) {
            error = L"Could not create an update package parent directory safely";
            return false;
        }
        WinHandle output(CreateFileW(destinationPath.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!output) {
            const DWORD createError = GetLastError();
            error = L"Could not create update package file " +
                    destinationPath.wstring() + L" (Windows error " +
                    std::to_wstring(createError) + L")";
            return false;
        }
        OutputContext outputContext{output.get()};
        if (!mz_zip_reader_extract_to_callback(
                &zip, entry.m_file_index, &WriteExtractedFile,
                &outputContext, 0) || !FlushFileBuffers(output.get())) {
            error = L"Update archive file extraction failed";
            return false;
        }
        LARGE_INTEGER outputSize{};
        if (!GetFileSizeEx(output.get(), &outputSize) || outputSize.QuadPart < 0 ||
            static_cast<std::uint64_t>(outputSize.QuadPart) != entry.m_uncomp_size) {
            error = L"Extracted update file length differs from its ZIP record";
            return false;
        }
        output.reset();
        const auto digest = Sha256File(destinationPath, error);
        const auto expected = artifact.files.find(path);
        if (!digest || expected == artifact.files.end() || *digest != expected->second) {
            if (error.empty()) error = L"Extracted update file hash differs from the manifest";
            return false;
        }
    }

    if (!VerifyManagedFiles(destination, artifact, error) ||
        !HasOnlyPortableRootEntries(destination) ||
        !ValidateVersionRecord(destination, targetVersion, error)) {
        if (error.empty()) error = L"Extracted portable package layout is invalid";
        return false;
    }
    return true;
}

bool ValidateInstalledRoot(const std::filesystem::path& root,
                           std::wstring& error) {
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        !HasOnlyPortableRootEntries(root)) {
        error = L"Current portable install root is missing or has an unexpected layout";
        return false;
    }
    return true;
}

bool MovePath(const std::filesystem::path& source,
              const std::filesystem::path& destination) {
    return MoveFileExW(source.c_str(), destination.c_str(),
                       MOVEFILE_WRITE_THROUGH) != FALSE;
}

void RemoveOwnedDirectory(const std::filesystem::path& path) {
    std::error_code error;
    if (!path.empty() && std::filesystem::is_directory(path, error) && !error) {
        std::filesystem::remove_all(path, error);
    }
}

bool IsInside(const std::filesystem::path& child,
              const std::filesystem::path& parent) {
    const auto childText = std::filesystem::absolute(child).lexically_normal().wstring();
    auto parentText = std::filesystem::absolute(parent).lexically_normal().wstring();
    if (parentText.empty()) return false;
    if (parentText.back() != L'\\' && parentText.back() != L'/') parentText.push_back(L'\\');
    return childText.size() >= parentText.size() &&
           CompareStringOrdinal(childText.data(), static_cast<int>(parentText.size()),
               parentText.data(), static_cast<int>(parentText.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

PortableUpdateResult PortableUpdateApply::Apply(
    const std::filesystem::path& stagedArchive,
    const std::filesystem::path& derivedInstallRoot,
    std::wstring_view targetVersion,
    const UpdateArtifact& artifact,
    IPortableUpdateHost& host) {
    PortableUpdateResult result;
    std::wstring error;
    if (stagedArchive.empty() || derivedInstallRoot.empty() ||
        !stagedArchive.is_absolute() || !derivedInstallRoot.is_absolute() ||
        !IsValidSemVer(targetVersion) ||
        IsInside(stagedArchive, derivedInstallRoot)) {
        result.error = L"Update source and install root must be absolute and separate";
        return result;
    }
    const DWORD stagedAttributes = GetFileAttributesW(stagedArchive.c_str());
    if (stagedAttributes == INVALID_FILE_ATTRIBUTES ||
        (stagedAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                             FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        result.error = L"Staged update archive is missing or is not a regular file";
        return result;
    }
    const auto digest = Sha256File(stagedArchive, error);
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(stagedArchive, sizeError);
    if (!digest || sizeError || *digest != artifact.sha256 ||
        size != artifact.sizeBytes) {
        result.error = error.empty() ? L"Staged update archive failed integrity verification" : error;
        return result;
    }
    if (!HasRequiredFiles(artifact, error)) {
        result.error = std::move(error);
        return result;
    }
    if (!ValidateInstalledRoot(derivedInstallRoot, error)) {
        result.error = std::move(error);
        return result;
    }

    const auto parent = derivedInstallRoot.parent_path();
    std::filesystem::path prepared;
    std::filesystem::path backup;
    if (parent.empty() || !MakeUniqueSibling(parent, L".CampusNetClient-update-", prepared)) {
        result.error = L"Could not create a protected sibling update directory";
        return result;
    }
    if (!ExtractValidatedArchive(stagedArchive, prepared, targetVersion, artifact,
                                 error)) {
        result.error = std::move(error);
        RemoveOwnedDirectory(prepared);
        return result;
    }
    if (!UniqueSiblingName(parent, L".CampusNetClient-backup-", backup)) {
        result.error = L"Could not reserve a protected rollback directory";
        RemoveOwnedDirectory(prepared);
        return result;
    }

    bool serviceInstalled = false;
    if (!host.QueryServiceInstallation(serviceInstalled, error)) {
        result.error = error.empty() ? L"Could not query the CampusNet service" : error;
        RemoveOwnedDirectory(prepared);
        return result;
    }
    if (serviceInstalled && !host.StopService(error)) {
        const std::wstring stopError = error;
        host.StartService(error);
        result.error = stopError.empty() ? L"Could not stop the CampusNet service" : stopError;
        RemoveOwnedDirectory(prepared);
        return result;
    }
    if (!host.CloseClientProcesses(error)) {
        const std::wstring closeError = error;
        if (serviceInstalled) host.StartService(error);
        result.error = closeError.empty() ? L"Could not close the CampusNet client" : closeError;
        RemoveOwnedDirectory(prepared);
        return result;
    }

    if (!MovePath(derivedInstallRoot, backup)) {
        error = L"Could not move the current install to its rollback directory";
        if (serviceInstalled) host.StartService(error);
        result.error = std::move(error);
        RemoveOwnedDirectory(prepared);
        return result;
    }
    if (!MovePath(prepared, derivedInstallRoot)) {
        const DWORD moveError = GetLastError();
        const bool restored = MovePath(backup, derivedInstallRoot);
        if (serviceInstalled) host.StartService(error);
        result.rolledBack = restored;
        result.recoveryDirectory = restored ? std::filesystem::path{} : backup;
        result.error = L"Could not place the verified update (Windows error " +
                       std::to_wstring(moveError) + L")";
        if (!restored) result.error += L"; previous files remain in " + backup.wstring();
        RemoveOwnedDirectory(prepared);
        return result;
    }

    bool serviceStarted = !serviceInstalled ||
        (host.StartService(error) &&
         host.WaitForServiceRunning(kServiceStartTimeout, error));
    if (!serviceStarted) {
        const std::wstring applyError = error.empty()
            ? L"Updated service did not become healthy" : error;
        std::filesystem::path failedNew;
        UniqueSiblingName(parent, L".CampusNetClient-failed-", failedNew);
        if (serviceInstalled) host.StopService(error);
        const bool movedNew = !failedNew.empty() &&
                              MovePath(derivedInstallRoot, failedNew);
        const bool restored = movedNew && MovePath(backup, derivedInstallRoot);
        bool oldServiceStarted = !serviceInstalled;
        if (restored && serviceInstalled) {
            oldServiceStarted = host.StartService(error) &&
                host.WaitForServiceRunning(kServiceStartTimeout, error);
        }
        result.rolledBack = restored && oldServiceStarted;
        result.recoveryDirectory = result.rolledBack ? std::filesystem::path{} : backup;
        result.error = applyError;
        if (!result.rolledBack) {
            result.error += L"; rollback was incomplete, recovery files: " + backup.wstring();
        } else {
            RemoveOwnedDirectory(failedNew);
        }
        return result;
    }

    RemoveOwnedDirectory(backup);
    result.success = true;
    return result;
}
