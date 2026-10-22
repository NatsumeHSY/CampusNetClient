#include "UpdateManifest.h"
#include "UpdateIntegrity.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

int RunUpdateIntegrityTests() {
    constexpr std::wstring_view abcHash =
        L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

    const auto uniqueSuffix = std::to_wstring(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = std::filesystem::temp_directory_path() /
                      (L"campusnet-update-integrity-test-" + uniqueSuffix);
    std::filesystem::create_directories(root / L"bin");
    const auto file = root / L"bin" / L"CampusNetClient.exe";
    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "abc";
        assert(output.good());
    }

    std::wstring error;
    const auto digest = Sha256File(file, error);
    assert(digest.has_value());
    assert(*digest == abcHash);

    UpdateArtifact artifact;
    artifact.files.emplace(L"bin/CampusNetClient.exe", std::wstring(abcHash));
    assert(VerifyManagedFiles(root, artifact, error));

    RemoteUpdateManifest remote;
    remote.version = L"1.2.3";
    remote.portable.files = artifact.files;
    LocalInstallInfo local{L"1.2.3", UpdateChannel::Portable};
    std::map<std::wstring, std::wstring> localDigests = artifact.files;
    assert(DecideUpdate(local, remote, localDigests) == UpdateDecision::UpToDate);

    remote.version = L"1.2.2";
    assert(DecideUpdate(local, remote, localDigests) ==
           UpdateDecision::InstallRemoteVersion);

    remote.version = L"1.2.3";
    localDigests[L"bin/CampusNetClient.exe"] = std::wstring(64, L'0');
    assert(DecideUpdate(local, remote, localDigests) == UpdateDecision::RepairFiles);

    localDigests = artifact.files;
    localDigests.clear();
    assert(DecideUpdate(local, remote, localDigests) == UpdateDecision::RepairFiles);

    local.version = L"1.2.3+local";
    remote.version = L"1.2.3";
    assert(DecideUpdate(local, remote, artifact.files) ==
           UpdateDecision::InstallRemoteVersion);

    {
        std::ofstream output(file, std::ios::binary | std::ios::trunc);
        output << "tampered";
    }
    assert(!VerifyManagedFiles(root, artifact, error));

    std::filesystem::remove_all(root);
    return 0;
}
