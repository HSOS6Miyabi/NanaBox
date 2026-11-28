# NanaBox Configuration File Reference

## Overview

- File Extension: .7b
- Save Encoding: UTF-8 with BOM as mandatory
- Configuration Format: JSON (JavaScript Object Notation)
- JSON Schema: https://raw.githubusercontent.com/M2Team/NanaBox/main/Documents/ConfigurationSchema.json

```
{
    "NanaBox": {
        "Type": "VirtualMachine",
        "Version": 1,
        "GuestType": "Windows",
        "Name": "DemoVM",     
        "ProcessorCount": 4,
        "MemorySize": 2048,
        "ComPorts": {
            "UefiConsole": "ComPort1",
            "ComPort1": "\\\\.\\pipe\\vmpipe1",
            "ComPort2": "\\\\.\\pipe\\vmpipe2"
        },
        "Gpu": {
            "AssignmentMode": "List",
            "SelectedDevices": [
                "\\\\?\\PCI#VEN_10DE&DEV_1C82&SUBSYS_11BF1B4C&REV_A1#4&38ab2860&0&0008#{064092b3-625e-43bf-9eb5-dc845897dd59}\\GPUPARAV"
            ]
        },     
        "NetworkAdapters": [
            {
                "Connected": true,
                "MacAddress": "00-15-5D-64-2F-AB",
                "EndpointId": "f2288275-6c30-47d4-bc24-293fa9c9cb12"
            }
        ],
        "ScsiDevices": [
            {
                "Type": "VirtualDisk",
                "Path": "Test.vhdx"
            },
            {
                "Type": "VirtualImage",
                "Path": ""
            }
        ],
        "SecureBoot": true,
        "Tpm": true,
        "GuestStateFile": "Test.vmgs",
        "RuntimeStateFile": "Test.vmrs",
        "SaveStateFile": "Test.SaveState.vmrs",
        "ExposeVirtualizationExtensions": true
    }
}
```

## Reference

- NanaBox (Object)
  - Type (String)
  - Version (Number)
  - GuestType (String)
  - Name (String)
  - ProcessorCount (Number)
  - MemorySize (Number)
  - ComPorts (Object)
    - UefiConsole (String)
    - ComPort1 (String)
    - ComPort2 (String)
  - Gpu (Object)
    - AssignmentMode (String)
    - EnableHostDriverStore (Boolean)
    - SelectedDevices (String or Object Array)
      - DeviceInterface (String, Object Array Only)
      - PartitionId (String, Object Array Only)
  - NetworkAdapters (Object Array)
    - Connected (Boolean)
    - MacAddress (String)
    - EndpointId (String)
    - SuggestedSwitchId (String)
    - SuggestedSwitchName (String)
    - SuggestedSwitchSubnet (String)
  - ScsiDevices (Object Array)
    - Type (String)
    - Path (String)
  - SecureBoot (Boolean)
  - Tpm (Boolean)
  - GuestStateFile (String)
  - RuntimeStateFile (String)
  - SaveStateFile (String)
  - ExposeVirtualizationExtensions (Boolean)
  - Keyboard (Object)
    - RedirectKeyCombinations (Boolean)
    - FullScreenHotkey (Number)
    - CtrlEscHotkey (Number)
    - AltEscHotkey (Number)
    - AltTabHotkey (Number)
    - AltShiftTabHotkey (Number)
    - AltSpaceHotkey (Number)
    - CtrlAltDelHotkey (Number)
    - FocusReleaseLeftHotkey (Number)
    - FocusReleaseRightHotkey (Number)
  - EnhancedSession (Object)
    - RedirectAudio (Boolean)
    - RedirectAudioCapture (Boolean)
    - RedirectDrives (Boolean)
    - RedirectPrinters (Boolean)
    - RedirectPorts (Boolean)
    - RedirectSmartCards (Boolean)
    - RedirectClipboard (Boolean)
    - RedirectDevices (Boolean)
    - RedirectPOSDevices (Boolean)
    - RedirectDynamicDrives (Boolean)
    - RedirectDynamicDevices (Boolean)
    - Drives (String Array)
    - Devices (String Array)
  - ChipsetInformation (Object)
    - BaseBoardSerialNumber (String)
    - ChassisSerialNumber (String)
    - ChassisAssetTag (String)
    - Manufacturer (String)
    - ProductName (String)
    - Version (String)
    - SerialNumber (String)
    - Family (String)
    - UUID (String)
    - SKUNumber (String)
  - VideoMonitor (Object)
    - HorizontalResolution (Number)
    - VerticalResolution (Number)
    - DisableBasicSessionDpiScaling (Boolean)
  - Policies (String Array)
  - Plan9Shares (Object Array)
    - ReadOnly (Boolean)
    - Port (Number)
    - Path (String)
    - Name (String)
  - VirtualSmbShares (Object Array)
    - ReadOnly (Boolean)
    - AutoMount (Boolean)
    - NoDirectmap (Boolean)
    - Path (String)
    - Name (String)

### NanaBox

The parent object for all types of NanaBox Configuration File.

### Type

The type of configuration.

Available values: "VirtualMachine"

### Version

The version of virtual machine configuration. Only 1 is available.

Available values: 1

### GuestType

The guest OS type of virtual machine.

Available values: "Windows", "Linux" and "Unknown"

### Name

The unique name of virtual machine.

Example value: "TestVM"

### ProcessorCount

The processor count of virtual machine, in cores.

Example value: 2

### MemorySize

The memory size of virtual machine, in MB.

Example value: 2048

Note: You can update the memory size at runtime starting with NanaBox 1.1.

### Resource allocation

The following optional settings are applied when creating the virtual machine. Change them while the VM is stopped. A zero limit or weight leaves the corresponding HCS setting unspecified.

| Setting | Meaning | Default |
| --- | --- | --- |
| ProcessorLimit | CPU scheduling limit in units of 1/1000 of one percent (100000 = 100%). | 0 |
| ProcessorWeight | Relative scheduling weight, 1–10000. | 0 |
| AllowMemoryOvercommit | Back guest memory with the Windows pagefile. | true |
| EnableDeferredCommit | Charge memory commit on first access; requires AllowMemoryOvercommit. | false |
| StorageIopsMaximum | Maximum storage I/O operations per second. | 0 |
| StorageBandwidthMaximum | Maximum storage bandwidth in bytes per second. | 0 |

### ComPorts

The COM ports setting object of virtual machine.

#### UefiConsole

The UEFI serial console setting of virtual machine.

Available values: "Disabled", "Default", "ComPort1" and "ComPort2"

#### ComPort1

(Optional) The named pipe path of guest COM1 of virtual machine.

Example value: "\\\\.\\pipe\\vmpipe1"

Note: You can update the named pipe path at runtime starting with NanaBox 1.1.

#### ComPort2

(Optional) The named pipe path of guest COM2 of virtual machine.

Example value: "\\\\.\\pipe\\vmpipe2"

Note: You can update the named pipe path at runtime starting with NanaBox 1.1.

### Gpu

The GPU setting object of virtual machine. You can choose GPU-P (partitioning)
and/or GPU-PV (paravirtualization) modes for every specific GPU instance.

Note: Before NanaBox 1.3 Update 1, you can only use GPU-PV (paravirtualization)
mode for virtual machines.

Note: You can update GPU setting at runtime starting with NanaBox 1.1.

#### AssignmentMode

The GPU assignment mode setting of virtual machine.

Available values: "Disabled", "Default", "List" and "Mirror"

- "Disabled" mode: Do not assign any GPU to the virtual machine.
- "Default" mode: Assign the single default GPU with GPU-PV (paravirtualization)
  mode to virtual machine, which currently is POST GPU.
- "List" mode: Assign the GPU(s)/partition(s) specified in SelectedDevices to
  virtual machine. If SelectedDevices is empty, do not assign GPU to the virtual
  machine.
- "Mirror" mode: Assign all current and future GPUs with GPU-PV
  (paravirtualization) mode to virtual machine.

#### EnableHostDriverStore

(Optional) Set it true if you want to enable readonly Plan 9 and Virtual SMB
shares for several GPU-PV (paravirtualization) related folders from the Host OS.
Linux guests receive the driver FileRepository and lxss/lib shares over Plan 9.
Windows guests receive the Windows driver store shares, without Linux ELF libraries.
Unknown guests retain both transport types for compatibility.

Note: You cannot suspend the virtual machine properly if this option is enabled.

Note: All these shares are transported with Hyper-V Socket.

Note: Available starting with NanaBox 1.2.

Here are the exposed readonly Plan 9 and Virtual SMB shares:

- Access Name: HostDriverStore
  - Available starting with NanaBox 1.2.
  - Deprecated, reserved for compatibility with virtual machines hosted with
    previous NanaBox versions.
  - Host Path: %SystemRoot%\System32\DriverStore
  - Hyper-V Socket Port for Plan 9 share: 50001
  - Virtual SMB share support: N/A
  - Suggested Guest Paths:
    - Windows: %SystemRoot%\System32\HostDriverStore
- Access Name: NanaBox.HostDrivers
  - Available starting with NanaBox 1.5 Update 3.
  - Host Path: %SystemRoot%\System32\DriverStore\FileRepository
  - Hyper-V Socket Port for Plan 9 share: 50001
  - Virtual SMB share support: Available starting with NanaBox 1.6 Update 1.
  - Suggested Guest Paths:
    - Windows: %SystemRoot%\System32\HostDriverStore\FileRepository
    - Linux: /usr/lib/wsl/drivers
- Access Name: NanaBox.HostLxssLib
  - Available starting with NanaBox 1.5 Update 3.
  - Host Path: %SystemRoot%\System32\lxss\lib
  - Hyper-V Socket Port for Plan 9 share: 50001
  - Virtual SMB share support: Available starting with NanaBox 1.6 Update 1.
  - Suggested Guest Paths:
    - Linux: /usr/lib/wsl/lib

#### SelectedDevices

(Optional) The string or object array of selected GPUs used for virtual machine.
Set as the object with "DeviceInterface" and "PartitionId" for the specific GPU
instance if you want to use GPU-P (partitioning) mode, or set as the string
which contains "DeviceInterface" for using GPU-PV (paravirtualization) mode.

Note: Only valid in "List" GPU paravirtualization assignment mode.

Note: Before NanaBox 1.3 Update 1, you can only use GPU-PV (paravirtualization)
mode for virtual machines.

Reference: https://docs.microsoft.com/en-us/windows/win32/hyperv_v2/msvm-partitionablegpu

##### DeviceInterface

The device interface path of the selected GPU used for paravirtualization of
virtual machine.

Example value: "\\\\?\\PCI#VEN_10DE&DEV_1C82&SUBSYS_11BF1B4C&REV_A1#4&38ab2860&0&0008#{064092b3-625e-43bf-9eb5-dc845897dd59}\\GPUPARAV"

##### PartitionId

The partition ID of the selected GPU used for paravirtualization of virtual
machine. It's actually an uint16 value. Set it 65535 for using GPU-PV 
(paravirtualization) mode.

### NetworkAdapters

The network adapters setting object array of virtual machine.

Note: You can add, remove and update network adapters at runtime starting with
NanaBox 1.1.

Note: Starting with NanaBox 1.5 Update 1, NanaBox will use the switch with the
name "NanaBox" and the GUID "85C8668E-36FE-447F-AAE0-BA159A0142A7" created by
NanaBox itself as the default switch, instead of Hyper-V Default Switch to
improve the stability.

#### Connected

Make the current network adapter connected if set it true.

#### MacAddress

(Optional) The MAC address of the current network adapter.

Note: If value not set, NanaBox will generate a new one for it.

Example value: "00-15-5D-64-2F-AB"

#### EndpointId

(Optional) The Endpoint GUID of the current network adapter.

Note: If value not set, NanaBox will generate a new one for it.

Note: This option is used for internal implementation.

Example value: "f2288275-6c30-47d4-bc24-293fa9c9cb12"

#### SuggestedSwitchId (String)

(Optional) The switch GUID suggested for the current network adapter.

Note: Available starting with NanaBox 1.6 Update 1.

Note: This value will not be used if this value is empty, invalid or have the
same GUID as the default switch created by NanaBox itself.

Note: Default behavior is use the default switch.

Example value: "85c8668e-36fe-447f-aae0-ba159a0142a7"

#### SuggestedSwitchName (String)

(Optional) The switch name suggested for the current network adapter.

Note: Available starting with NanaBox 1.6 Update 1.

Note: This value will not be used if this value is invalid or SuggestedSwitchId
is not be used for the current network adapter.

Note: If SuggestedSwitchId is used but this value is empty, NanaBox will use the
"NanaBox_[SuggestedSwitchId]" as the switch name.

Note: Default behavior is use the default switch.

Example value: "Custom Switch"

#### SuggestedSwitchSubnet (String)

(Optional) The switch subnet suggested for the current network adapter.

Note: Available starting with NanaBox 1.6 Update 1.

Note: This value will not be used if this value is empty, invalid, or the
switch used for the current network adapter has been created before.

Note: Default behavior is use the default subnet setting of the switch.

Example value: "172.22.78.0/24"

### ScsiDevices

The SCSI devices setting object array of virtual machine.

Note: You can add and update SCSI devices at runtime starting with NanaBox 1.1.

#### Type

The type of the current SCSI device.

Available values: "VirtualDisk", "VirtualImage" and "PhysicalDevice"

#### Path

The path of the current SCSI device.

Note: The relative path is supported.

When type is "VirtualDisk", you can use vhdx and vhd files.

When type is "VirtualImage", you can use iso files, and you can make it empty or
not set if you want to make a ejected virtual optical drive.

When type is "PhysicalDevice", you can expose your physical drive to virtual 
machine. you can set it something like "\\\\.\\PhysicalDriveX" where X is an 
integer that represents the particular enumeration of the physical disk on the 
caller's system.

### SecureBoot

(Optional) The Secure Boot setting of virtual machine.

If you want to enable Secure Boot for your virtual machine, please set it true.

### Tpm

(Optional) The Trusted Platform Module (TPM) setting of virtual machine.

If you want to enable Trusted Platform Module (TPM) for your virtual machine,
please set it true.

Note: Available starting with NanaBox 1.2 Update 2.

Note: Only the Trusted Platform Module (TPM) 2.0 is supported.

Note: You need Windows 11 Version 24H2 or later Host OS. (Although Windows
Server 2022 had introduced the related Host Compute System API interfaces,
but it seems doesn't be implemented.)

### GuestStateFile

(Optional) The path of guest state file for virtual machine.

Note: The relative path is supported.

Note: If value not set or file not exist, NanaBox will create a new one for it.

Example value: "TestVM.vmgs"

### RuntimeStateFile

(Optional) The path of runtime state file for virtual machine.

Note: The relative path is supported.

Note: If value not set or file not exist, NanaBox will create a new one for it.

Example value: "TestVM.vmrs"

### SaveStateFile

(Optional) The path of save state file for virtual machine.

Note: The relative path is supported.

Note: This option is used for internal implementation.

Example value: "TestVM.SaveState.vmrs"

### ExposeVirtualizationExtensions

(Optional) Expose the virtualization extensions to the virtual machine if set
it true.

Note: Some processors don't support exposing the virtualization extensions to
the virtual machine.

### Keyboard

(Optional) Keyboard setting object of virtual machine.

For more information about the default keyboard shortcut behavior, please read
https://learn.microsoft.com/en-us/windows/win32/termserv/terminal-services-shortcut-keys.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### ClipboardKeyboardLayout

The clipboard typing button uses physical keyboard scan codes. Select the layout active in the guest: `00000409` (US, default), `00000809` (UK), `00000411` (Japanese), `00000407` (German), `0000040C` (French), or any installed Windows keyboard layout identifier (KLID). `Host` uses the current host layout. The complete text is validated before sending any keys. Printable characters representable by the layout, tabs and line breaks are supported.

#### ClipboardGtkUnicode

Optional, default `false`. Enable for Linux guests when the focused application supports GTK numeric Unicode input. Characters not directly representable by the selected keyboard layout are entered using Ctrl+Shift+U, their hexadecimal Unicode code point and Enter. Supplementary-plane characters are decoded from UTF-16 surrogate pairs. This setting requires a compatible input method, such as GtkIMContextSimple; leave it disabled for firmware consoles, terminals without the input method, and other unsupported applications. See [GTK Unicode input](https://docs.gtk.org/gtk4/class.IMContextSimple.html).

#### ClipboardWindowsUnicode

Optional, default `false`. For Windows guests, install GuestTool with `InstallWindowsGuest.ps1 -EnableUnicodeInput` and enable this setting to use Windows Unicode keyboard input. The active console session is selected in basic mode; enhanced mode requires exactly one active Remote Desktop session. Input is sent to the focused application on the ordinary interactive desktop using the logged-on user's token. Locked sessions, secure desktops and applications that reject injected input are unsupported. The service does not use guest clipboard redirection. Leave this option disabled for pre-login and firmware input.

#### RedirectKeyCombinations

(Optional) Apply key combinations at the virtual machine if set it true, or
apply key combinations to the virtual machine only when the host is running in
full-screen mode.

If you don't want to apply key combinations at the virtual machine, please set
it false.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### FullScreenHotkey

(Optional) Specifies the virtual-key code to add to CTRL+ALT to determine the
hotkey replacement for switching to full-screen mode.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### CtrlEscHotkey

(Optional) Specifies the virtual-key code to add to ALT to determine the hotkey
replacement for CTRL+ESC.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### AltEscHotkey

(Optional) Specifies the virtual-key code to add to ALT to determine the hotkey
replacement for ALT+ESC.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### AltTabHotkey

(Optional) Specifies the virtual-key code to add to ALT to determine the hotkey
replacement for ALT+TAB.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### AltShiftTabHotkey

(Optional) Specifies the virtual-key code to add to ALT to determine the hotkey
replacement for ALT+SHIFT+TAB.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### AltSpaceHotkey

(Optional) Specifies the virtual-key code to add to ALT to determine the hotkey
replacement for ALT+SPACE.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### CtrlAltDelHotkey

(Optional) Specifies the virtual-key code to add to CTRL+ALT to determine the
hotkey replacement for CTRL+ALT+DELETE, also called the secure attention
sequence (SAS).

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### FocusReleaseLeftHotkey

(Optional) Specifies the virtual-key code to add to Ctrl+Alt to determine the
hotkey replacement for Ctrl+Alt+Left Arrow.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

#### FocusReleaseRightHotkey

(Optional) Specifies the virtual-key code to add to Ctrl+Alt to determine the
hotkey replacement for Ctrl+Alt+Right Arrow.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

Note: You need to use the decimal value of the virtual-key code.

Note: For more information about virtual-key code, please read 
https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes.

### EnhancedSession

(Optional) Enhanced session setting object of virtual machine.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectAudio

(Optional) Redirect sounds from the virtual machine to the host if set it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectAudioCapture

(Optional) Redirect audio capture from the host to the virtual machine if set it
true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectDrives

(Optional) Redirect all disk drives from the host to the virtual machine if set
it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectPrinters

(Optional) Redirect all printers from the host to the virtual machine if set it
true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectPorts

(Optional) Redirect all local ports (for example, COM and LPT) from the host to
the virtual machine if set it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectSmartCards

(Optional) Redirect all smart cards from the host to the virtual machine if set
it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectClipboard

(Optional) Redirect clipboard from the host to the virtual machine if set it
true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectDevices

(Optional) Redirect all devices from the host to the virtual machine if set it
true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectPOSDevices

(Optional) Redirect all Point of Service devices from the host to the virtual
machine if set it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectDynamicDrives

(Optional) Redirect all dynamically attached Plug and Play (PnP) drives that are
enumerated while virtual machine running if set it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### RedirectDynamicDevices

(Optional) Redirect all dynamically attached Plug and Play (PnP) devices that
are enumerated while virtual machine running if set it true.

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### Drives

(Optional) The string array of selected disk drives used for redirection from
the host to the virtual machine.

Example value: "C"

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

#### Devices

(Optional) The string array of selected devices used for redirection from the
host to the virtual machine.

Example value: "USB\\VID_5986&PID_211C&MI_00\\6&218C4A3&0&0000"

Note: Available starting with NanaBox 1.1 and you can modify these settings at
runtime.

### ChipsetInformation

The chipset information object of virtual machine.

Note: Available starting with NanaBox 1.2 Update 4.

#### BaseBoardSerialNumber

The serial number of the baseboard of virtual machine.

Example value: "QTFCOU0000001"

Note: Available starting with NanaBox 1.2 Update 4.

#### ChassisSerialNumber

The serial number of the chassis of virtual machine.

Example value: "QTFCOU0000001"

Note: Available starting with NanaBox 1.2 Update 4.

#### ChassisAssetTag 

The asset tag of the chassis of virtual machine.

Example value: "TestVM"

Note: Available starting with NanaBox 1.2 Update 4.

#### Manufacturer

The manufacturer of virtual machine.

Leave blank to use default vaule: "Microsoft Corporation"

Example value: "NVIDIA"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### ProductName

The product name of virtual machine.

Leave blank to use default vaule: "Virtual Machine"

Example value: "DGX-1"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### Version

The version of virtual machine.

Leave blank to use default vaule: "Hyper-V UEFI Release v4.1"

Example value: "DGX-1 with V100-32 1.0"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### SerialNumber

The serial number of virtual machine.

Leave blank to use default vaule

Example value: "QTFCOU0000001"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### Family

The family of virtual machine.

Leave blank to use default vaule: "Virtual Machine"

Example value: "Enterprise Server"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### UUID

The universally unique identifier of virtual machine.

Example value: "12345678-abcd-efgh-ijkl-0123456789ab"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

#### SKUNumber

The sku number of virtual machine.

Example value: "TestVM"

Note: You need Windows Server 2022 or later Host OS.

Note: Available starting with NanaBox 1.2 Update 4.

### VideoMonitor

The video monitor setting object of virtual machine.

Note: Available starting with NanaBox 1.5.

#### HorizontalResolution

(Optional) The horizontal resolution of the video monitor of virtual machine.

Example value: 1920

Note: Only the Basic Session Mode is constrainted with this setting.

Note: Available starting with NanaBox 1.5.

#### VerticalResolution

(Optional) The vertical resolution of the video monitor of virtual machine.

Example value: 1080

Note: Only the Basic Session Mode is constrainted with this setting.

Note: Available starting with NanaBox 1.5.

#### DisableBasicSessionDpiScaling

(Optional) Disable the DPI scaling for the Basic Session Mode of virtual
machine if set it true.

Note: Available starting with NanaBox 1.5 Update 2 and you can modify these
settings at runtime.

### Policies

The string array of policies of virtual machine.

Note: Available starting with NanaBox 1.5.

#### Available Policies starting with NanaBox 1.5

Note: These policies need Windows 11 Version 24H2 or later Host OS.

- LegacyPcrMeasurement
- DisableSha384Pcr
- LegacyRteWidth
- LegacyTrustedLaunchMemoryMap
- LegacyApicSelectionLogic
- LegacyApicManganeseOverride
- LegacyGpaLayout
- ForceLegacyHcl
- ForceCurrentHcl
- UseExtendedMaxVpPerSocketLimit

Example: If you want to do some hacks to boot x86 (64-Bit) Windows XP / Vista /
7 on Hyper-V Generation 2 Virtual Machines with NanaBox and Windows 11 Version
24H2 or later Host OS, you can set the following policies to help you do further
researches.

- LegacyRteWidth
- LegacyApicSelectionLogic
- LegacyApicManganeseOverride
- LegacyGpaLayout

### Plan9Shares

The Plan 9 shares setting object array of virtual machine.

Note: You cannot suspend the virtual machine properly if this option is enabled.

Note: All these shares are transported with Hyper-V Socket.

Note: Available starting with NanaBox 1.5 Update 2.

#### ReadOnly

(Optional) Set it true if you want to make the current Plan 9 share read-only.

Note: Available starting with NanaBox 1.5 Update 2.

#### Port

The Hyper-V Socket port number of the current Plan 9 share.

Example value: 50001

Note: Available starting with NanaBox 1.5 Update 2.

#### Path

The host path of the current Plan 9 share.

Note: The relative path is supported.

Example value: "D:\\Projects"

Note: Available starting with NanaBox 1.5 Update 2.

#### Name

The guest access name of the current Plan 9 share.

Example value: "HostProjects"

Note: If you have set the "EnableHostDriverStore" true, you should not use the
names mentioned in "EnableHostDriverStore" related document for the current Plan
9 share.

Note: Available starting with NanaBox 1.5 Update 2.

### VirtualSmbShares

The Virtual SMB shares setting object array of virtual machine.

Note: You cannot suspend the virtual machine properly if this option is enabled.

Note: Available starting with NanaBox 1.6 Update 1.

#### ReadOnly

(Optional) Set it true if you want to make the current Virtual SMB share read-only.

Note: Available starting with NanaBox 1.6 Update 1.

#### AutoMount

Optional, defaults to true. Automatically mount this share in a Windows guest running the native GuestTool share service. When false, the share remains available for manual mounting.

#### NoDirectmap

Optional, defaults to false. Set to true to disable HCS direct file mapping for this share. This option is independent of read-only access and GuestTool directory mounting.

#### Path

The host path of the current Virtual SMB share.

Note: The relative path is supported.

Example value: "D:\\Projects"

Note: Available starting with NanaBox 1.6 Update 1.

#### Name

The guest access name of the current Virtual SMB share.

Example value: "HostProjects"

Note: If you have set the "EnableHostDriverStore" true, you should not use the
names mentioned in "EnableHostDriverStore" related document for the current
Virtual SMB share.

Note: Available starting with NanaBox 1.6 Update 1.

## Samples

### Typical Windows Virtual Machine

```
{
  "NanaBox": {
    "ComPorts": {
      "UefiConsole": "Disabled"
    },
    "Gpu": {
      "AssignmentMode": "Mirror"
    },
    "GuestStateFile": "TestVM.vmgs",
    "GuestType": "Windows",
    "MemorySize": 4096,
    "Name": "TestVM",
    "NetworkAdapters": [
      {
        "Connected": true,
        "EndpointId": "cd67ed54-c64d-4c98-bc0a-6d5f38ca43ce",
        "MacAddress": "00-15-5D-64-2F-AB"
      }
    ],
    "ProcessorCount": 2,
    "RuntimeStateFile": "TestVM.vmrs",
    "ScsiDevices": [
      {
        "Path": "Windows 11.vhdx",
        "Type": "VirtualDisk"
      },
      {
        "Type": "VirtualImage"
      }
    ],
    "SecureBoot": true,
    "Type": "VirtualMachine",
    "Version": 1
  }
}
```

### Typical Ubuntu Virtual Machine

```
{
  "NanaBox": {
    "ComPorts": {
      "ComPort1": "",
      "ComPort2": "",
      "UefiConsole": "Disabled"
    },
    "Gpu": {
      "AssignmentMode": "Mirror"
    },
    "GuestType": "Linux",
    "MemorySize": 4096,
    "Name": "UbuntuVM",
    "NetworkAdapters": [
      {
        "Connected": true,
        "EndpointId": "594f35de-4d8e-4173-8e9f-00f2b372d2f4",
        "MacAddress": "00-15-5D-47-EB-71"
      }
    ],
    "ProcessorCount": 2,
    "ScsiDevices": [
      {
        "Path": "D:\\NanaBox VM\\UbuntuVM\\UbuntuVM.vhdx",
        "Type": "VirtualDisk"
      },
      {
        "Path": "D:\\Updates\\ubuntu-22.04-desktop-amd64.iso",
        "Type": "VirtualImage"
      }
    ],
    "Type": "VirtualMachine",
    "Version": 1
  }
}
```

### Typical UEFI Application Debugging Virtual Machine

```
{
  "NanaBox": {
    "ComPorts": {
      "ComPort1": "\\\\.\\pipe\\vmpipe1",
      "ComPort2": "\\\\.\\pipe\\vmpipe2",
      "UefiConsole": "ComPort1"
    },
    "Gpu": {
      "AssignmentMode": "Disabled"
    },
    "GuestStateFile": "UefiTestVM.vmgs",
    "GuestType": "Windows",
    "MemorySize": 512,
    "Name": "UefiTestVM",
    "NetworkAdapters": [
      {
        "Connected": true,
        "EndpointId": "d811c915-5421-4e31-afbf-5db608e7ad24",
        "MacAddress": "00-15-5D-47-EB-0D"
      }
    ],
    "ProcessorCount": 1,
    "RuntimeStateFile": "UefiTestVM.vmrs",
    "ScsiDevices": [
      {
        "Path": "D:\\NanaBox VM\\UEFIDebugDisk.vhdx",
        "Type": "VirtualDisk"
      }
    ],
    "Type": "VirtualMachine",
    "Version": 1
  }
}
```

### Typical NVIDIA DGX-1 Virtual Machine (In order to bypass the installation detection of the DGX BaseOS)

```
{
  "NanaBox": {
    "ChipsetInformation": {
      "Family": "Enterprise Server",
      "Manufacturer": "NVIDIA",
      "ProductName": "DGX-1",
      "SerialNumber": "QTFCOU0000001",
      "Version": "DGX-1 with V100-32 1.0"
    },
    "ComPorts": {
      "UefiConsole": "Disabled"
    },
    "Gpu": {
      "AssignmentMode": "List",
      "SelectedDevices": [
        "\\\\?\\PCI#VEN_10DE&DEV_1DB5&SUBSYS_12491215&REV_A1#4&38ab2860&0&0008#{064092b3-625e-43bf-9eb5-dc845897dd59}\\GPUPARAV"
      ]
    },
    "GuestType": "Linux",
    "MemorySize": 16384,
    "Name": "DGX OS",
    "NetworkAdapters": [
      {
        "Connected": true,
        "EndpointId": "594f35de-4d8e-4173-8e9f-00f2b372d2f4",
        "MacAddress": "00-15-5D-47-EB-71"
      }
    ],
    "ProcessorCount": 8,
    "ScsiDevices": [
      {
        "Path": "D:\\NanaBox VM\\DGX OS.vhdx",
        "Type": "VirtualDisk"
      },
      {
        "Path": "D:\\Updates\\DGXOS-5.6.0-2024-04-16-06-58-50.iso",
        "Type": "VirtualImage"
      }
    ],
    "Type": "VirtualMachine",
    "Version": 1
  }
}
```

### Explicit HCN networks

Each network adapter may specify `NetworkId`, the GUID of an existing HCN network. This takes precedence over the suggested switch options. The specified network must exist; opening it never falls back to a different network.

Optionally specify `FirewallCreatorId`, the GUID of a registered Hyper-V firewall VM creator on a firewall-enabled HCN network. NanaBox requests the HCN PortName and Firewall endpoint policies. Configure the creator's rules using Windows Hyper-V firewall tools. Windows and the network must support these policies.

### Mirrored networking

Set `MirroredNetworking` to `true` on a compatible Windows host with the WSL networking dependency installed. Install the NanaBox GuestTool in the guest and configure its mirrored network adapters as dedicated interfaces. HCN provides the host network endpoints; the guest service synchronizes addresses, interface metrics, routes, DNS and ephemeral port ranges. The current synchronization state is shown in the virtual machine window title.

`MirroredAutoReservePorts` defaults to `true`. The service periodically detects guest listening TCP and bound UDP ports and requests their HCN reservations. Set it to `false` to use only explicit reservations. `MirroredTcpPorts` and `MirroredUdpPorts` are arrays of port numbers from 1 to 65535, reserved before the guest starts. Use explicit reservations for services that require a port before the first synchronization cycle. Port conflicts are displayed in the status. Automatic reservation is periodic and does not intercept each bind operation.

`MirroredDnsTunneling` (default `false`) sends DNS queries to the Windows host resolver through Hyper-V sockets. Enable DNS tunneling when installing the guest service as well. See the [guest service guide](../GuestTool/README.md).

`MirroredFirewallCreatorId` optionally selects a registered Hyper-V firewall VM creator GUID. Configure its policy and rules in Host management. A compatible HCN/Hyper-V firewall implementation is required. Saving VM state temporarily removes dynamic mirrored interfaces and releases GPU assignments. On restoration, NanaBox creates mirrored endpoints again and restores the configured GPU assignments; connections using removed interfaces may need to reconnect.

### Display options

`VideoMonitor.EnableDpiScalingValueOverride` enables `OverriddenDpiScalingValue` (100–500 percent). `DisableBasicSessionDpiScaling` takes precedence for Basic Session. With `EnableContentResizing` disabled, the enhanced-session desktop uses `HorizontalResolution` and `VerticalResolution` and scales to fit the window instead of changing the guest desktop resolution. `ShowFullScreenModeConnectionBar` controls the RDP connection bar. Existing configurations retain automatic scaling, desktop resizing, and the connection bar by default.

The Network settings page provides host preflight checks, live mirrored network status, manual synchronization requests, and copying diagnostic reports. The host checks inspect available HCN APIs, the WSL dependency file and network enumeration without creating networks. The live report records the endpoint IDs, MAC addresses, mirrored configuration, guest acknowledgments, last successful synchronization, port reservations and conflicts. `MirroredIgnoredPorts` excludes ports from automatic listener reservation outside the dynamic port range and must not overlap explicitly reserved ports. The connection-specific DNS suffix is synchronized alongside DNS server addresses.


### Registered Hyper-V VM management

The Hyper-V VMs tab operates on VMMS-registered virtual machines using the local Hyper-V PowerShell module. The dynamic-memory operation sets minimum, startup and maximum memory, buffer and priority. Migration uses Move-VM and the current host credentials; specify a destination host and optionally an absolute storage path on that host. Shared storage is used when the destination storage path is empty. Configure live migration and authentication on both hosts before starting the operation.

Hyper-V Replica operations configure the destination server, port, Kerberos or certificate authentication, compression, replication frequency and recovery history. Initial replication, suspension, resumption and removal are separate operations. Removing a replication relationship locally does not remove the relationship on the remote host. Configure the Replica server before enabling replication. The inspection result includes replication information. Standalone HCS virtual machines are not VMMS-registered virtual machines.

### Advanced HCS creation properties

The Advanced tab exposes `HcsSchemaMinor` (the major version is 2, the default minor version is 1) and `AdditionalHcsSettings`, a JSON object merged into the generated HCS `VirtualMachine` document before creation. For example, `{"Chipset":{"UseUtc":true,"IsNumLockDisabled":true}}` adds documented chipset options. Configure the required HCS schema version for the desired feature and consult the [Microsoft HCS schema reference](https://learn.microsoft.com/en-us/virtualization/api/hcs/schemareference). NanaBox rejects collisions with fields it manages, including case-insensitive duplicates, and preserves its disk, NIC, display, GPU, guest state, and share provisioning. Additional devices may require separate host resource preparation. The installed host validates whether a requested feature is supported when the VM is created. These advanced properties apply on the next VM creation, not to a running VM.


The Hyper-V VM manager can also configure processor reservation, limit, weight and nested virtualization; attach and detach virtual disks, set per-disk IOPS, add DVD drives and mount or eject ISO files; add, remove and connect network adapters, configure static or dynamic MAC addresses, access VLANs, DHCP and router guards, MAC spoofing; enable or disable individual integration services; and set Generation 2 Secure Boot. Inspect the VM to load current device identities and switches. A disk detach leaves the backing file untouched. Device and switch identities are checked against the selected VM or current host before each change.


## HCN resources

The HCN resources settings tab lists and inspects networks, endpoints, namespaces, load balancers and SDN routes using the installed HCN API. Create and modify resources with native HCN JSON settings; IDs are assigned by NanaBox. The separate network tab provides simplified network creation. For a standalone endpoint, choose the ID of a NanaBox-managed network, then use `{"SchemaVersion":{"Major":2,"Minor":0}}` with any required endpoint properties. A standalone endpoint is not automatically attached to a VM. Create a namespace with `{"Type":"Host"}` or `{"Type":"Guest"}`, then add or remove an endpoint using `{"ResourceType":"Endpoint","RequestType":"Add","Settings":{"EndpointId":"<managed endpoint GUID>"}}`. Network and endpoint policy modifications use `{"ResourceType":"Policy","RequestType":"Add","Settings":{"Policies":[...]}}`. Load balancers and routes use their native creation settings and `HostComputeEndpoints` containing managed endpoint IDs. Inspect existing resources to obtain settings and IDs. SDN route changes require deleting and recreating the route. Resource manager ownership of endpoints, namespaces, load balancers and routes is recorded in the local machine registry; unrelated system resources are read-only. The available HCN APIs and schema versions depend on Windows and the HCN service.

### Plan 9 guest mounts and advanced options

Plan9 shares are configured by name, port, host directory and access mode. `AutoMount` (default `true`) mounts shares under `/media/<Name>` in a Linux guest with the NanaBox Plan 9 service installed. Turn it off to mount on demand. Names must be unique (case-insensitive) and valid directory components without commas or path separators. The default port is 50001; ports 27453–27457 are reserved for guest services. Multiple shares can use one port. `LinuxMetadata`, `CaseSensitive`, `UseShareRootIdentity`, `AllowOptions`, `AllowSubPaths` and `UnlimitedConnections` select optional HCS flags and default to false. `AllowedFiles` restricts the share to specified file names and raises the generated HCS schema minor version to at least 2. See the [guest tools guide](../GuestTool/README.md).

For a running Linux VM, the Plan 9 settings are editable. NanaBox uses the HCS `VirtualMachine/Devices/Plan9/Shares` modification resource. Removing or replacing a share requires the guest service to unmount it first; if the mount is busy, the original share is retained. After modifying the provider, NanaBox publishes the actual share inventory. Changes to automatic mounting do not recreate the provider. Use the guest service's mount commands for managed mounts so that they can be safely removed.

### Windows Virtual SMB mounts

For Windows guests, the native GuestTool share service mounts configured Virtual SMB shares under `%ProgramData%\NanaBox Guest Shares\<share>`. The Guest tools can mount and unmount individual configured shares. Manual unmount suppresses subsequent automatic mounts and removes only the local Bind Filter mapping, leaving the HCS export and transport intact. When GPU driver sharing is enabled, the FileRepository and restricted supplemental shares use the same VMBus transport with read-only Bind Filter mappings. Host Virtual SMB share definitions apply at the next VM creation. Linux guests use their kernel Plan 9 client. See the GuestTool installation guide for native and compatibility mode installation and maintenance.
