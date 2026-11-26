# NanaBox automated tests

The Windows build runs configuration/HCS request self-tests and creates, resizes, compacts, and queries a temporary VHDX using the production disk APIs. `Tests/SettingsUI.ps1` drives the real settings editor through Windows UI Automation: it changes controls, adds/removes GPU, network, disk and share rows, saves a temporary .7b file, checks the serialized values, reloads, and tests the all-GPU mode. The test does not create or start a VM. It requires a Windows desktop session with a working XAML Islands/UI Automation environment.

Run the UI test from Windows PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/SettingsUI.ps1 -Executable Output/Binaries/Root/Binaries/x64/NanaBox.exe
```

The packaged executable must have `resources.pri` and its XAML resource file beside it. The editor can also be opened directly using `NanaBox.exe --edit-config path.7b`.

The host-network mutation, GPU-PV, live VM power controls and guest integration tests require a compatible Hyper-V host and disposable test guests. Never run destructive VM/network tests against production VMs or networks.
