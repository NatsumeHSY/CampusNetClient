#include "CredentialStore.h"

#include <cassert>

namespace {

class MemoryCredentialStore final : public ICredentialStore {
public:
    std::optional<Credentials> Read() override { return value; }
    bool Write(const Credentials& credentials) override {
        value = credentials;
        return true;
    }
    bool Clear() override {
        value.reset();
        return true;
    }

private:
    std::optional<Credentials> value;
};

} // namespace

int RunCredentialStoreTests() {
    MemoryCredentialStore store;
    assert(!store.Read().has_value());
    assert(store.Write({L"student", L"secret-value"}));
    const auto loaded = store.Read();
    assert(loaded.has_value());
    assert(loaded->username == L"student");
    assert(loaded->password == L"secret-value");
    assert(store.Clear());
    assert(!store.Read().has_value());
    return 0;
}
