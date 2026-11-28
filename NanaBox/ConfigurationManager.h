/*
 * PROJECT:    NanaBox
 * FILE:       ConfigurationManager.h
 * PURPOSE:    Definition for the Virtual Machine Configuration Manager
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#ifndef NANABOX_CONFIGURATION_MANAGER
#define NANABOX_CONFIGURATION_MANAGER

#include "NanaBox.Configuration.Specification.h"

#include "HostCompute.h"
#include "RdpClient.h"

#include <Mile.Json.h>

namespace NanaBox
{
    void ValidateScsiDeviceConfiguration(
        winrt::hstring const& HcsVmId,
        ScsiDeviceConfiguration const& Configuration);

    nlohmann::json MakeHcsComPortConfiguration(
        std::string const& NamedPipe);

    nlohmann::json MakeHcsNetworkAdapterConfiguration(
        NetworkAdapterConfiguration const& Configuration);

    nlohmann::json MakeHcsScsiDeviceConfiguration(
        ScsiDeviceConfiguration const& Configuration);

    void ValidatePlan9Shares(VirtualMachineConfiguration const& Configuration);
    nlohmann::json MakeHcsPlan9Share(Plan9ShareConfiguration const& Share);
    bool EqualPlan9ShareName(Plan9ShareConfiguration const& Left, Plan9ShareConfiguration const& Right);
    void ValidatePlan9HostDirectory(Plan9ShareConfiguration const& Share);
    nlohmann::json MakeHcsPlan9Modification(Plan9ShareConfiguration const& Share, bool Add);
    void ComputeSystemModifyPlan9Share(winrt::com_ptr<ComputeSystem> const& Instance,
        Plan9ShareConfiguration const& Share, bool Add);
    nlohmann::json MakeHcsVirtualSmbShare(VirtualSmbShareConfiguration const& Share);
    nlohmann::json MakeHcsVirtualSmbAddition(VirtualSmbShareConfiguration const& Share);
    bool EqualVirtualSmbShareName(VirtualSmbShareConfiguration const& Left, VirtualSmbShareConfiguration const& Right);
    void ValidateVirtualSmbHostDirectory(VirtualSmbShareConfiguration const& Share);
    void ComputeSystemAddVirtualSmbShare(winrt::com_ptr<ComputeSystem> const& Instance,
        VirtualSmbShareConfiguration const& Share);
    nlohmann::json MakePlan9GuestManifest(VirtualMachineConfiguration const& Configuration,
        nlohmann::json const& HcsConfiguration);

    std::string MakeHcsConfiguration(
        VirtualMachineConfiguration const& Configuration);

    void ComputeNetworkCreateEndpoint(
        std::string const& Owner,
        NetworkAdapterConfiguration& Configuration);

    void ComputeNetworkDeleteEndpoint(
        std::string const& Owner,
        NetworkAdapterConfiguration const& Configuration);

    void NormalizeNetworkAdapterIds(std::vector<NetworkAdapterConfiguration>& Adapters);

    void ComputeSystemUpdateMemorySize(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint64_t const& MemorySize);

    void ComputeSystemAddComPort(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint32_t const& PortID,
        std::string const& NamedPipe);

    void ComputeSystemRemoveComPort(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint32_t const& PortID,
        std::string const& NamedPipe);

    void ComputeSystemUpdateComPort(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint32_t const& PortID,
        std::string const& NamedPipe);

    void ComputeSystemAddNetworkAdapter(
        winrt::com_ptr<ComputeSystem> const& Instance,
        NetworkAdapterConfiguration const& Configuration);

    void ComputeSystemRemoveNetworkAdapter(
        winrt::com_ptr<ComputeSystem> const& Instance,
        NetworkAdapterConfiguration const& Configuration);

    void ComputeSystemAddScsiDevice(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint32_t const& DeviceID,
        ScsiDeviceConfiguration const& Configuration);

    void ComputeSystemUpdateScsiDevice(
        winrt::com_ptr<ComputeSystem> const& Instance,
        std::uint32_t const& DeviceID,
        ScsiDeviceConfiguration const& Configuration);

    nlohmann::json MakeHcsGpuModification(GpuConfiguration const& Configuration);

    void ComputeSystemUpdateGpu(
        winrt::com_ptr<ComputeSystem> const& Instance,
        GpuConfiguration const& Configuration);

    void RemoteDesktopUpdateKeyboardConfiguration(
        winrt::com_ptr<RdpClient> const& Instance,
        KeyboardConfiguration& Configuration);

    void RemoteDesktopUpdateEnhancedSessionConfiguration(
        winrt::com_ptr<RdpClient> const& Instance,
        EnhancedSessionConfiguration& Configuration);

    VirtualMachineConfiguration DeserializeConfiguration(
        std::string const& Configuration);

    std::string SerializeConfiguration(
        VirtualMachineConfiguration const& Configuration);
}

#endif // !NANABOX_CONFIGURATION_MANAGER
