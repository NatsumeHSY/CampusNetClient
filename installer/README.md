# CampusNetClient installer

`CampusNetClient.wxs` is the WiX v4 source for the unpackaged deployment. It installs the WinUI 3 client and `CampusNetService.exe` into the same directory so the service can launch the client in the active user session without command-line arguments.

The MSI registers `CampusNetService` as a native Windows Service with automatic startup. Uninstall stops and removes the service through MSI service actions. The product itself does not expose a public CLI, `cmd.exe`, PowerShell, scheduled task, or VBS entry point.

The source was compiled locally with WiX 4.0.6 using the Debug binaries. `build-msi.ps1` writes the MSI and its WiX metadata to the workspace-root `build\package\` by default; keep generated packages out of the workspace-level `release` directory, which belongs to the original Python client. The validation package is a packaging smoke-test artifact, not a signed release package. Before publishing, rebuild with the Release output and provide these variables:

- `ProductVersion`: four-part MSI version.
- `ClientBinDir`: directory containing the client executable, XAML binary resources, merged WinMD, and WebView2 Core files.
- `ServiceBinDir`: directory containing `CampusNetService.exe`.

Example developer build command (the installed product still exposes no public CLI):

```powershell
wix build CampusNetClient.wxs `
  -d ProductVersion=0.1.0.0 `
  -d ClientBinDir=C:\path\to\client\bin `
  -d ServiceBinDir=C:\path\to\service\bin `
  -o ..\..\build\package\CampusNetClient-0.1.0.0.msi
```

The Evergreen WebView2 Runtime is a machine prerequisite and is not embedded in this MSI. The first-run UI should report a missing runtime as a WebView2 fallback failure rather than silently falling back to a shell or browser command.

