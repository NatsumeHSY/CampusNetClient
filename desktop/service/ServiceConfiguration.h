#pragma once

#include <Windows.h>

#include <string>

bool ConfigureCampusNetService(std::wstring& error);
bool ConfigureCampusNetServiceHandle(SC_HANDLE service, std::wstring& error);
