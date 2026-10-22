#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>

int RunUpdaterAgent(HANDLE stopEvent, HANDLE checkEvent,
                    const std::filesystem::path& stagingDirectory);
int RunUpdaterAgentFromEnvironment();
