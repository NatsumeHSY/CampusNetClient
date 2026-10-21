#pragma once

#include <optional>
#include <string>

struct Credentials {
    std::wstring username;
    std::wstring password;
};

class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;

    virtual std::optional<Credentials> Read() = 0;
    virtual bool Write(const Credentials& credentials) = 0;
    virtual bool Clear() = 0;
};

class WindowsCredentialStore final : public ICredentialStore {
public:
    explicit WindowsCredentialStore(
        std::wstring target = L"CampusNetClient/credentials");

    std::optional<Credentials> Read() override;
    bool Write(const Credentials& credentials) override;
    bool Clear() override;

private:
    std::wstring target_;
};
