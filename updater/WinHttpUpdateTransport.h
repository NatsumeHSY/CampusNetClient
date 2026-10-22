#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

struct HttpLimits {
    std::uint64_t maxBytes = 0;
    std::uint32_t totalTimeoutMs = 0;
    std::uint32_t readTimeoutMs = 0;
    std::uint32_t maxRedirects = 0;
};

struct HttpResponse {
    std::uint32_t statusCode = 0;
    std::uint64_t contentLength = 0;
    std::wstring finalUrl;
    std::string body;
};

using ChunkSink =
    std::function<bool(std::span<const std::uint8_t> chunk)>;
using ProgressCallback = std::function<void(std::uint64_t receivedBytes,
                                            std::uint64_t totalBytes,
                                            std::uint64_t bytesPerSecond)>;

class IUpdateTransport {
public:
    virtual ~IUpdateTransport() = default;

    virtual bool Get(std::wstring_view url, const HttpLimits& limits,
                     HttpResponse& response, std::wstring& error) = 0;
    virtual bool Download(std::wstring_view url, const HttpLimits& limits,
                          const ChunkSink& sink,
                          const ProgressCallback& progress,
                          std::wstring& error) = 0;
};

enum class UpdateUrlPurpose {
    Manifest,
    Artifact,
    ArtifactRedirect,
};

bool IsAllowedUpdateUrl(std::wstring_view url, UpdateUrlPurpose purpose);

class WinHttpUpdateTransport final : public IUpdateTransport {
public:
    bool Get(std::wstring_view url, const HttpLimits& limits,
             HttpResponse& response, std::wstring& error) override;
    bool Download(std::wstring_view url, const HttpLimits& limits,
                  const ChunkSink& sink,
                  const ProgressCallback& progress,
                  std::wstring& error) override;
};
