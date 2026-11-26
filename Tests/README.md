# NanaBox automated tests

The Windows build runs configuration/HCS request self-tests and creates, resizes, compacts, and queries a temporary VHDX using the production disk APIs. `Tests/SettingsUI.ps1` drives the real settings editor through Windows UI Automation: it changes controls, adds/removes GPU, network, disk and share rows, saves a temporary .7b file, checks the serialized values, reloads, and tests the all-GPU mode. The test does not create or start a VM. It requires a Windows desktop session with a working XAML Islands/UI Automation environment.

Run the UI test from Windows PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/SettingsUI.ps1 -Executable Output/Binaries/Root/Binaries/x64/NanaBox.exe
```

The packaged executable must have `resources.pri` and its XAML resource file beside it. The editor can also be opened directly using `NanaBox.exe --edit-config path.7b`.

The host-network mutation, GPU-PV, live VM power controls and guest integration tests require a compatible Hyper-V host and disposable test guests. Never run destructive VM/network tests against production VMs or networks.

The settings test executes VHDX creation, expansion, compaction and inspection in the storage page and checks the returned disk sizes. With `-HostIntegration`, it also creates and deletes a uniquely named private Hyper-V switch, registers an isolated Hyper-V firewall creator, configures its policy, creates/disables/enables/deletes a rule, and creates/inspects/deletes an HCN NAT network. Use an elevated, interactive, dedicated Windows Hyper-V runner with NetSecurity's Hyper-V firewall cmdlets available. The manually dispatched hardware workflow enables this mode. External switch creation is not exercised on shared network adapters.

The hardware workflow uses two disposable VM configurations with GPU-PV and mirrored networking enabled, the guest service installed, and a reachable TCP test server. It verifies the exact HCS Running/Paused states, successful HCN synchronization, actual guest GPU identity, a guest address matching a host address, TCP connectivity using a mirrored source address, and clean guest shutdown. The GPU regex inputs must identify the actual expected GPU; for a Linux dxg guest, a pattern such as `"Dxg"\\s*:\\s*true` is accepted. Do not point the workflow at production VMs or network services.
