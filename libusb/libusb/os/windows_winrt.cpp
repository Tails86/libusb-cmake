/*
 * winrt backend for libusb 1.0
 * Copyright © 2025 James Smith <jmsmith86@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libusbi.h"
#include "windows_winrt.hpp" // TODO: remove

#include <unordered_map>
#include <string>
#include <sstream>
#include <iomanip>
#include <functional>
#include <future>
#include <unordered_set>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Usb.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Devices::Usb;
using namespace winrt::Windows::Storage;

static constexpr const uint8_t SIMULATED_IMANUFACTURER = 1;
static constexpr const uint8_t SIMULATED_IPRODUCT = 2;
static constexpr const uint8_t SIMULATED_ISERIAL = 3;

// TODO: this could be moved into winrt_context_priv
static std::mutex container_id_to_session_id_mutex;
static std::unordered_map<guid, unsigned long> container_id_to_session_id_map;
static unsigned long last_session_id = 0;

static unsigned long container_id_to_session_id(const guid& container_id)
{
    std::lock_guard<std::mutex> lock(container_id_to_session_id_mutex);

    auto iter = container_id_to_session_id_map.find(container_id);
    if (iter == container_id_to_session_id_map.end())
    {
        unsigned long new_session_id = ++last_session_id;
        container_id_to_session_id_map.insert(std::make_pair(container_id, new_session_id));
        return new_session_id;
    }

    return iter->second;
}

//! Safely retrieves the result of an winrt async get()
//! @tparam T The result type to be retrieved
//! @param[in] getFn A function which both creates an async operation and calls get()
//! @return The retrieved value
template <typename T>
static T winrt_async_get(const std::function<T()>& getFn)
{
    try
    {
        // Synopsis:
        // winrt uses the UI message pump when current thread is UI thread. This will usually mean that the asynchronous
        // operation will rely on messaging. Blocking while on this thread would then cause a deadlock because messages
        // won't be handled. To avoid that particular case, the async operation will be executed in its own thread so
        // winrt-internal operations don't rely on the message pump.
        if (winrt::impl::is_sta_thread())
        {
            std::future<T> task = std::async(std::launch::async, getFn);
            return task.get();
        }
        else
        {
            return getFn();
        }
    }
    catch(const winrt::hresult_error& e)
    {
        // Execution error occurred
        // TODO: need to properly handle and log this error elsewhere
        // printf("winrt_async_get failed: %s\n", winrt::to_string(e.message()).c_str());
        return nullptr;
    }
}

static int winrt_init(struct libusb_context *ctx)
{
    winrt_context_priv *priv = new (usbi_get_context_priv(ctx)) winrt_context_priv();
    return LIBUSB_SUCCESS;
}

static void winrt_exit(struct libusb_context *ctx)
{
}

static int winrt_set_option(struct libusb_context *ctx, enum libusb_option option, va_list ap)
{
    UNUSED(ctx);
    UNUSED(option);
    UNUSED(ap);

    //winrt_context_priv *priv = static_cast<winrt_context_priv*>(usbi_get_context_priv(ctx));

    // TODO: Nothing probably needs to be supported here

    return LIBUSB_ERROR_NOT_SUPPORTED;
}

static int winrt_get_device_list(struct libusb_context *ctx, struct discovered_devs **_discdevs)
{
    // Find all connected USB devices
    auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    additionalProperties.Append(L"System.Devices.ContainerId");
    additionalProperties.Append(L"System.Devices.DeviceInstanceId");
    DeviceInformationCollection deviceInfos = winrt_async_get<DeviceInformationCollection>(
        [&]()
        {
            return DeviceInformation::FindAllAsync(
                L"System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
                L" AND (System.Devices.DeviceInstanceId:~<\"USB\\\" OR System.Devices.DeviceInstanceId:~<\"HID\\\")",
                additionalProperties,
                DeviceInformationKind::DeviceInterface
            ).get();
        }
    );

    std::unordered_set<guid> foundContainerIds;

    for (const DeviceInformation& deviceInfo : deviceInfos)
    {
        guid containerId;
        deviceInfo.Properties().Lookup(L"System.Devices.ContainerId").as(containerId);
        hstring deviceInstanceId;
        // For debug purposes
        // deviceInfo.Properties().Lookup(L"System.Devices.DeviceInstanceId").as(deviceInstanceId);
        // std::string wDeviceInstanceId = winrt::to_string(deviceInstanceId);
        // printf("DeviceInstanceId: %s\n", wDeviceInstanceId.c_str());
        if (foundContainerIds.count(containerId) == 0)
        {
            unsigned long session_id = container_id_to_session_id(containerId);
            libusb_device *dev = usbi_get_device_by_session_id(ctx, session_id);

            if (dev == NULL) {
                dev = usbi_alloc_device(ctx, session_id);
                if (dev == NULL)
                {
                    return LIBUSB_ERROR_NO_MEM;
                }

                dev->bus_number = (session_id >> 8) & 0xFF; // TODO: is this specified anywhere?
                dev->device_address = session_id & 0xFF; // TODO: is this specified anywhere?
                dev->speed = libusb_speed::LIBUSB_SPEED_UNKNOWN; // TODO: is this specified anywhere?

                // Use placement new to properly construct the private structure
                winrt_device_priv *dpriv = new (usbi_get_device_priv(dev)) winrt_device_priv();
                // Save the container ID to device priv data for later use
                dpriv->container_id = winrt::to_hstring(containerId);

                UsbDevice winrtDev = winrt_async_get<UsbDevice>(
                    [&deviceInfo]()
                    {
                        return UsbDevice::FromIdAsync(deviceInfo.Id()).get();
                    }
                );

                if (!winrtDev)
                {
                    // Only 1 application may hold this device at one time, so it is likely in use - can't be parsed
                    libusb_unref_device(dev);
                    continue;
                }

                // winrtDev.DeviceDescriptor() does not contain all data of the device descriptor.
                // Instead, send control transfer to get device descriptor.
                auto setupPacket = UsbSetupPacket();
                setupPacket.RequestType().Direction(UsbTransferDirection::In);
                setupPacket.RequestType().ControlTransferType(UsbControlTransferType::Standard);
                setupPacket.RequestType().Recipient(UsbControlRecipient::Device);
                setupPacket.Request(LIBUSB_REQUEST_GET_DESCRIPTOR);
                setupPacket.Value((LIBUSB_DT_DEVICE << 8) | 0); // Device descriptor, index 0
                setupPacket.Index(0);
                setupPacket.Length(LIBUSB_DT_DEVICE_SIZE);

                auto outputBuffer = Streams::Buffer(LIBUSB_DT_DEVICE_SIZE);
                auto buffer = winrt_async_get<Streams::IBuffer>(
                    [&]()
                    {
                        return winrtDev.SendControlInTransferAsync(setupPacket, outputBuffer).get();
                    }
                );

                if (!buffer) {
                    libusb_unref_device(dev);
                    continue;
                }

                auto dataReader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer);
                std::vector<uint8_t> data(buffer.Length());
                dataReader.ReadBytes(data);

                // Copy the raw descriptor data from buffer to device descriptor
                if (data.size() >= LIBUSB_DT_DEVICE_SIZE)
                {
                    memcpy(&dev->device_descriptor, data.data(), LIBUSB_DT_DEVICE_SIZE);
                }
                usbi_localize_device_descriptor(&dev->device_descriptor);

                int err = usbi_sanitize_device(dev);
                if (err)
                {
                    libusb_unref_device(dev);
                    return err;
                }
            }

            if (discovered_devs_append(*_discdevs, dev) == NULL)
            {
                return LIBUSB_ERROR_NO_MEM;
            }

            libusb_unref_device(dev);

            foundContainerIds.insert(containerId);
        }
    }

    return LIBUSB_SUCCESS;
}

static inline uint16_t ReadLittleEndian16(const uint8_t p[2])
{
    return (uint16_t)((uint16_t)p[1] << 8 | (uint16_t)p[0]);
}

static int winrt_open(struct libusb_device_handle *dev_handle)
{
    // Use placement new to properly construct the private structure
    winrt_device_handle_priv *handle_priv = new (usbi_get_device_handle_priv(dev_handle)) winrt_device_handle_priv();
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev_handle->dev));

    // One DeviceInterface must be open to perform any device operation
    auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    DeviceInformationCollection deviceInfos = winrt_async_get<DeviceInformationCollection>(
        [&]()
        {
            return DeviceInformation::FindAllAsync(
                L"System.Devices.ContainerId:=\"" + priv->container_id + L"\"",
                additionalProperties,
                DeviceInformationKind::DeviceInterface
            ).get();
        }
    );

    if (deviceInfos.Size() == 0)
    {
        return LIBUSB_ERROR_NOT_FOUND;
    }

    bool commFail = false;
    for (const DeviceInformation& deviceInfo : deviceInfos)
    {
        UsbDevice winrtDev = winrt_async_get<UsbDevice>(
            [&deviceInfo]()
            {
                return UsbDevice::FromIdAsync(deviceInfo.Id()).get();
            }
        );

        if (winrtDev)
        {
            priv->default_device = winrtDev;
            priv->default_device_id = deviceInfo.Id();

            // Get the active configuration number
            auto setupPacket = UsbSetupPacket();
            setupPacket.RequestType().Direction(UsbTransferDirection::In);
            setupPacket.RequestType().ControlTransferType(UsbControlTransferType::Standard);
            setupPacket.RequestType().Recipient(UsbControlRecipient::Device);
            setupPacket.Request(LIBUSB_REQUEST_GET_CONFIGURATION);
            setupPacket.Value(0);
            setupPacket.Index(0);
            setupPacket.Length(1);

            auto outputBuffer = Streams::Buffer(LIBUSB_DT_CONFIG_SIZE);
            auto ibuf = winrt_async_get<Streams::IBuffer>(
                [&]()
                {
                    return winrtDev.SendControlInTransferAsync(setupPacket, outputBuffer).get();
                }
            );

            if (!ibuf)
            {
                commFail = true;
                // Try next device
                continue;
            }

            auto dataReader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(ibuf);
            std::vector<uint8_t> activeConfigData(1);
            dataReader.ReadBytes(activeConfigData);
            priv->active_config = activeConfigData[0];

            bool descRetrievalFailed = false;
            priv->config_descriptors.resize(dev_handle->dev->device_descriptor.bNumConfigurations);
            for (uint8_t i = 0; i < dev_handle->dev->device_descriptor.bNumConfigurations; ++i)
            {
                // The data within winrtDev.Configuration().Descriptors() is often incorrect for some reason.
                // The best bet is to simply send a control transfer.
                setupPacket = UsbSetupPacket();
                setupPacket.RequestType().Direction(UsbTransferDirection::In);
                setupPacket.RequestType().ControlTransferType(UsbControlTransferType::Standard);
                setupPacket.RequestType().Recipient(UsbControlRecipient::Device);
                setupPacket.Request(LIBUSB_REQUEST_GET_DESCRIPTOR);
                setupPacket.Value((LIBUSB_DT_CONFIG << 8) | i); // configuration descriptor with index
                setupPacket.Index(0);
                setupPacket.Length(LIBUSB_DT_CONFIG_SIZE);

                outputBuffer = Streams::Buffer(LIBUSB_DT_CONFIG_SIZE);
                ibuf = winrt_async_get<Streams::IBuffer>(
                    [&]()
                    {
                        return winrtDev.SendControlInTransferAsync(setupPacket, outputBuffer).get();
                    }
                );

                if (!ibuf)
                {
                    descRetrievalFailed = true;
                    break;
                }

                dataReader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(ibuf);
                priv->config_descriptors[i].resize(ibuf.Length());
                dataReader.ReadBytes(priv->config_descriptors[i]);

                // Get full length
                uint16_t realLen = ReadLittleEndian16(&priv->config_descriptors[i][2]);
                setupPacket.Length(realLen);
                outputBuffer = Streams::Buffer(realLen);
                ibuf = winrt_async_get<Streams::IBuffer>(
                    [&]()
                    {
                        return winrtDev.SendControlInTransferAsync(setupPacket, outputBuffer).get();
                    }
                );

                if (!ibuf)
                {
                    descRetrievalFailed = true;
                    break;
                }

                dataReader = winrt::Windows::Storage::Streams::DataReader::FromBuffer(ibuf);
                priv->config_descriptors[i].resize(ibuf.Length());
                dataReader.ReadBytes(priv->config_descriptors[i]);
            }

            if (descRetrievalFailed)
            {
                commFail = true;
                // Try next device
                continue;
            }

            return LIBUSB_SUCCESS;
        }
    }

    return commFail ? LIBUSB_ERROR_IO : LIBUSB_ERROR_BUSY;
}

static void winrt_close(struct libusb_device_handle *dev_handle)
{
    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(dev_handle));
    handle_priv->interfaces.clear();
    // Manually call destructor
    handle_priv->~winrt_device_handle_priv();
}

static int winrt_get_config_descriptor_by_value(
    libusb_device *dev,
    uint8_t bConfigurationValue,
    void **buffer
)
{
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev));

    if (bConfigurationValue == 0 || (bConfigurationValue - 1) >= priv->config_descriptors.size())
    {
        return -1;
    }

    *buffer = &priv->config_descriptors[bConfigurationValue - 1][0];
    return static_cast<int>(priv->config_descriptors[bConfigurationValue - 1].size());
}

static int winrt_get_active_config_descriptor(struct libusb_device *dev, void *buffer, size_t len)
{
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev));
    void *config_desc;

    int r = winrt_get_config_descriptor_by_value(dev, priv->active_config, &config_desc);
    if (r < 0)
        return r;

    len = MIN(len, (size_t)r);
    memcpy(buffer, config_desc, len);
    return (int)len;
}

static int winrt_get_config_descriptor(struct libusb_device *dev, uint8_t config_index, void *buffer, size_t len)
{
	winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev));

    if (config_index >= priv->config_descriptors.size())
    {
        return -1;
    }

    const uint8_t *config_header = &priv->config_descriptors[config_index][0];
    const std::size_t totalLength = priv->config_descriptors[config_index].size();

	len = MIN(len, totalLength);
	memcpy(buffer, config_header, len);
	return (int)len;
}

static int winrt_get_configuration(struct libusb_device_handle *dev_handle, uint8_t *config)
{
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev_handle->dev));

    *config = priv->active_config;
    return LIBUSB_SUCCESS;
}

static int winrt_set_configuration(struct libusb_device_handle *dev_handle, int config)
{
	winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev_handle->dev));

    if (!priv->default_device)
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    // Get the active configuration number
    auto setupPacket = UsbSetupPacket();
    setupPacket.RequestType().Direction(UsbTransferDirection::Out);
    setupPacket.RequestType().ControlTransferType(UsbControlTransferType::Standard);
    setupPacket.RequestType().Recipient(UsbControlRecipient::Device);
    setupPacket.Request(LIBUSB_REQUEST_SET_CONFIGURATION);
    setupPacket.Value(config);
    setupPacket.Index(0);
    setupPacket.Length(0);

    auto outputBuffer = Streams::Buffer(LIBUSB_DT_CONFIG_SIZE);
    auto ibuf = winrt_async_get<Streams::IBuffer>(
        [&]()
        {
            return priv->default_device.SendControlInTransferAsync(setupPacket, outputBuffer).get();
        }
    );

    if (!ibuf)
    {
        return LIBUSB_ERROR_IO;
    }

	priv->active_config = config;

	return LIBUSB_SUCCESS;
}

static int winrt_claim_interface(struct libusb_device_handle *dev_handle, uint8_t iface)
{
    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(dev_handle));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev_handle->dev));

    // For any communication, the default_device must be set
    if (!priv->default_device)
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    auto iter = handle_priv->interfaces.find(iface);
    if (iter != handle_priv->interfaces.end())
    {
        if (iter->second.device)
        {
            // Already claimed and valid
            return LIBUSB_SUCCESS;
        }
        else
        {
            // No longer valid, remove and retry
            handle_priv->interfaces.erase(iter);
        }
    }

    std::wstringstream ss;
    ss << std::setfill(L'0') << std::setw(2) << std::hex << static_cast<int>(iface);
    std::wstring ifaceStr = ss.str();

    auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    DeviceInformationCollection deviceInfos = winrt_async_get<DeviceInformationCollection>(
        [&]()
        {
            return DeviceInformation::FindAllAsync(
                L"System.Devices.ContainerId:=\"" + priv->container_id + L"\""
                L" AND System.Devices.DeviceInstanceId:~~\"MI_" + ifaceStr + L"\"",
                additionalProperties,
                DeviceInformationKind::DeviceInterface
            ).get();
        }
    );

    if (deviceInfos.Size() == 0)
    {
        return LIBUSB_ERROR_NOT_FOUND;
    }

    // There will usually only be 1 in the list unless the interface implements multiple DeviceInterfaceGUIDs.
    // Try connecting to each until one succeeds.
    for (const auto& deviceInfo : deviceInfos)
    {
        std::wstring id(deviceInfo.Id());
        UsbDevice winrtDev = nullptr;
        bool isDefaultDevice = false;

        if (id == priv->default_device_id)
        {
            winrtDev = priv->default_device;
            isDefaultDevice = true;
        }
        else
        {
            winrtDev = winrt_async_get<UsbDevice>(
                [&deviceInfo]()
                {
                    return UsbDevice::FromIdAsync(deviceInfo.Id()).get();
                }
            );
        }

        if (winrtDev)
        {
            winrt_interface itfDef{winrtDev, id};
            for (auto& bulkEpIn: winrtDev.DefaultInterface().BulkInPipes())
            {
                itfDef.bulk_in_pipes.insert_or_assign(bulkEpIn.EndpointDescriptor().EndpointNumber() | LIBUSB_ENDPOINT_IN, std::move(bulkEpIn));
            }
            for (auto& bulkEpOut: winrtDev.DefaultInterface().BulkOutPipes())
            {
                itfDef.bulk_out_pipes.insert_or_assign(bulkEpOut.EndpointDescriptor().EndpointNumber(), std::move(bulkEpOut));
            }
            for (auto& intEpIn: winrtDev.DefaultInterface().InterruptInPipes())
            {
                itfDef.interrupt_in_pipes.insert_or_assign(intEpIn.EndpointDescriptor().EndpointNumber() | LIBUSB_ENDPOINT_IN, std::move(intEpIn));
            }
            for (auto& intEpOut: winrtDev.DefaultInterface().InterruptOutPipes())
            {
                itfDef.interrupt_out_pipes.insert_or_assign(intEpOut.EndpointDescriptor().EndpointNumber(), std::move(intEpOut));
            }

            handle_priv->interfaces.insert(std::make_pair(iface, std::move(itfDef)));

            if (!isDefaultDevice)
            {
                // Save this as the default device if no control transfers are being processed
                std::lock_guard<std::mutex> lock(priv->control_transfers.transfer_mutex);
                if (!priv->control_transfers.active_transfer)
                {
                    priv->default_device = winrtDev;
                    priv->default_device_id = id;
                }
            }

            return LIBUSB_SUCCESS;
        }
    }

    // All found interfaces are busy
    return LIBUSB_ERROR_BUSY;
}

static int winrt_release_interface(struct libusb_device_handle *dev_handle, uint8_t iface)
{
    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(dev_handle));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev_handle->dev));

    // For any communication, the default_device must be set
    if (!priv->default_device)
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    auto iter = handle_priv->interfaces.find(iface);
    if (iter != handle_priv->interfaces.end())
    {
        bool updateDefaultDevice = (iter->second.device == priv->default_device);

        handle_priv->interfaces.erase(iter);

        if (updateDefaultDevice)
        {
            std::lock_guard<std::mutex> lock(priv->control_transfers.transfer_mutex);

            if (!priv->control_transfers.active_transfer)
            {
                // To avoid keeping an unused interface open just for basic operations, try to change the default device
                // to another used interface.
                for (const auto& claimedEntry : handle_priv->interfaces)
                {
                    if (claimedEntry.second.device)
                    {
                        priv->default_device = claimedEntry.second.device;
                        priv->default_device_id = claimedEntry.second.device_id;
                        break;
                    }
                }
            }
        }
    }

    return LIBUSB_SUCCESS;
}

static int winrt_set_interface_altsetting(struct libusb_device_handle *dev_handle, uint8_t iface, uint8_t altsetting)
{
    // TODO: implement
    return LIBUSB_ERROR_IO;
}

static int winrt_clear_halt(struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
    // TODO: implement
    return LIBUSB_ERROR_IO;
}

static int winrt_reset_device(struct libusb_device_handle *dev_handle)
{
    // TODO: implement
    return LIBUSB_ERROR_IO;
}

static void winrt_destroy_device(struct libusb_device *dev)
{
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev));
    if (priv->default_device)
    {
        priv->default_device.Close();
        priv->default_device = nullptr;
        priv->default_device_id.clear();
    }
    // Manually call destructor
    priv->~winrt_device_priv();
}

static int winrt_submit_control_transfer(struct usbi_transfer *itransfer)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(transfer->dev_handle->dev));
    libusb_control_setup *setup = (struct libusb_control_setup *)transfer->buffer;
    winrt_transfer_priv *transfer_priv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));

    auto setupPacket = UsbSetupPacket();
    // TODO: Clean up magic numbers here
    setupPacket.RequestType().Direction((setup->bmRequestType & 0x80) ? UsbTransferDirection::In : UsbTransferDirection::Out);
    setupPacket.RequestType().ControlTransferType(static_cast<UsbControlTransferType>((setup->bmRequestType& 0x60) >> 5));
    setupPacket.RequestType().Recipient(static_cast<UsbControlRecipient>(setup->bmRequestType & 0x1F));
    setupPacket.Request(setup->bRequest);
    setupPacket.Value(setup->wValue);
    setupPacket.Index(setup->wIndex);
    setupPacket.Length(setup->wLength);

    if (setup->bmRequestType & 0x80)
    {
        // IN transfer
        auto outputBuffer = Streams::Buffer(setup->wLength);
        auto asyncOp = priv->default_device.SendControlInTransferAsync(setupPacket, outputBuffer);

        // This is capturing by value to keep the reference back to async operation
        tpriv->cancel_fn = [asyncOp](){asyncOp.Cancel();};

        asyncOp.Completed([itransfer](auto const& sender, auto const& args) {
            libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
            libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
            if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Completed)
            {
                try {
                    auto buffer = sender.GetResults();
                    // // Debug: Print buffer contents as hex
                    // printf("Buffer length: %u, data: ", static_cast<unsigned int>(buffer.Length()));
                    // auto dataReader2 = Streams::DataReader::FromBuffer(buffer);
                    // std::vector<uint8_t> hexData(buffer.Length());
                    // dataReader2.ReadBytes(hexData);
                    // for (size_t i = 0; i < hexData.size(); i++) {
                    //     printf("%02x ", hexData[i]);
                    // }
                    // printf("\n");
                    if (buffer && buffer.Length() <= transfer->length - LIBUSB_CONTROL_SETUP_SIZE) {
                        auto dataReader = Streams::DataReader::FromBuffer(buffer);
                        dataReader.ReadBytes(winrt::array_view<uint8_t>(transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, buffer.Length()));
                        itransfer->transferred = buffer.Length();
                        status = LIBUSB_TRANSFER_COMPLETED;
                    }
                }
                catch (...) {}
            }
            else if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
            {
                status = LIBUSB_TRANSFER_CANCELLED;
            }

            winrt_transfer_completed(itransfer, status);
        });
    }
    else
    {
        // OUT transfer
        auto dataWriter = Streams::DataWriter();
        dataWriter.WriteBytes(winrt::array_view<const uint8_t>(transfer->buffer + LIBUSB_CONTROL_SETUP_SIZE, setup->wLength));
        auto inputBuffer = dataWriter.DetachBuffer();

        auto asyncOp = priv->default_device.SendControlOutTransferAsync(setupPacket, inputBuffer);

        // This is capturing by value to keep the reference back to async operation
        tpriv->cancel_fn = [asyncOp](){asyncOp.Cancel();};

        asyncOp.Completed([itransfer](auto const& sender, auto const& args) {
            libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
            libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
            if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Completed)
            {
                try {
                    auto bytesTransferred = sender.GetResults();
                    itransfer->transferred = bytesTransferred;
                    status = LIBUSB_TRANSFER_COMPLETED;
                }
                catch (...) {}
            }
            else if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
            {
                status = LIBUSB_TRANSFER_CANCELLED;
            }

            transfer->status = status;

            winrt_transfer_completed(itransfer, status);
        });
    }

    return LIBUSB_SUCCESS;
}

static int winrt_submit_bulk_transfer(struct usbi_transfer *itransfer)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));
    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(transfer->dev_handle));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(transfer->dev_handle->dev));
    winrt_transfer_priv *transfer_priv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));

    if (transfer->endpoint & 0x80)
    {
        // IN transfer
        auto outputBuffer = Streams::Buffer(transfer->length);

        for (std::pair<const uint8_t, winrt_interface>& itf : handle_priv->interfaces)
        {
            for (std::pair<const uint8_t, winrt::Windows::Devices::Usb::UsbBulkInPipe>& eps : itf.second.bulk_in_pipes)
            {
                if (eps.first == transfer->endpoint)
                {
                    auto asyncOp = eps.second.InputStream().ReadAsync(
                        outputBuffer,
                        transfer->length,
                        Streams::InputStreamOptions::Partial | Streams::InputStreamOptions::ReadAhead
                    );

                    // This is capturing by value to keep the reference back to async operation
                    tpriv->cancel_fn = [asyncOp](){asyncOp.Cancel();};

                    asyncOp.Completed([itransfer](auto const& sender, auto const& args) {
                        libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
                        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
                        if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Completed)
                        {
                            try {
                                auto buffer = sender.GetResults();
                                // // Debug: Print buffer contents as hex
                                // printf("Buffer length: %u, data: ", static_cast<unsigned int>(buffer.Length()));
                                // auto dataReader2 = Streams::DataReader::FromBuffer(buffer);
                                // std::vector<uint8_t> hexData(buffer.Length());
                                // dataReader2.ReadBytes(hexData);
                                // for (size_t i = 0; i < hexData.size(); i++) {
                                //     printf("%02x ", hexData[i]);
                                // }
                                // printf("\n");
                                if (buffer && buffer.Length() <= transfer->length) {
                                    auto dataReader = Streams::DataReader::FromBuffer(buffer);
                                    dataReader.ReadBytes(winrt::array_view<uint8_t>(transfer->buffer, buffer.Length()));
                                    itransfer->transferred = buffer.Length();
                                    status = LIBUSB_TRANSFER_COMPLETED;
                                }
                            }
                            catch (...) {}
                        }
                        else if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
                        {
                            status = LIBUSB_TRANSFER_CANCELLED;
                        }

                        winrt_transfer_completed(itransfer, status);
                    });

                    return LIBUSB_SUCCESS;
                }
            }
        }
    }
    else
    {
        // OUT transfer
        auto dataWriter = Streams::DataWriter();
        dataWriter.WriteBytes(winrt::array_view<const uint8_t>(transfer->buffer, transfer->length));
        auto inputBuffer = dataWriter.DetachBuffer();

        for (std::pair<const uint8_t, winrt_interface>& itf : handle_priv->interfaces)
        {
            for (std::pair<const uint8_t, winrt::Windows::Devices::Usb::UsbBulkOutPipe>& eps : itf.second.bulk_out_pipes)
            {
                if (eps.first == transfer->endpoint)
                {
                    auto asyncOp = eps.second.OutputStream().WriteAsync(inputBuffer);

                    // This is capturing by value to keep the reference back to async operation
                    tpriv->cancel_fn = [asyncOp](){asyncOp.Cancel();};

                    asyncOp.Completed([itransfer](auto const& sender, auto const& args) {
                        libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
                        libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
                        if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Completed)
                        {
                            try {
                                auto bytesTransferred = sender.GetResults();
                                itransfer->transferred = bytesTransferred;
                                status = LIBUSB_TRANSFER_COMPLETED;
                            }
                            catch (...) {}
                        }
                        else if (sender.Status() == winrt::Windows::Foundation::AsyncStatus::Canceled)
                        {
                            status = LIBUSB_TRANSFER_CANCELLED;
                        }

                        transfer->status = status;

                        winrt_transfer_completed(itransfer, status);
                    });

                    return LIBUSB_SUCCESS;
                }
            }
        }
    }

    return LIBUSB_ERROR_NOT_FOUND;
}

static int winrt_submit_transfer(struct usbi_transfer *itransfer)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
    // Use placement new to properly construct the private structure
    winrt_transfer_priv *tpriv = new (usbi_get_transfer_priv(itransfer)) winrt_transfer_priv();
    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(transfer->dev_handle));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(transfer->dev_handle->dev));

    // For any communication, the default_device must be set
    if (!priv->default_device)
    {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    // Only handling control transfers for now
    switch (transfer->type)
    {
        case LIBUSB_TRANSFER_TYPE_CONTROL:
        {
            std::lock_guard<std::mutex> lock(priv->control_transfers.transfer_mutex);

            if (priv->control_transfers.active_transfer)
            {
                // Still working on a transfer - will get to this transfer later
                priv->control_transfers.transfer_queue.push_back(itransfer);
                return LIBUSB_SUCCESS;
            }

            // Begin transfer now
            priv->control_transfers.active_transfer = itransfer;
            return winrt_submit_control_transfer(itransfer);
        }
        break;

        case LIBUSB_TRANSFER_TYPE_BULK: // Fall through
        case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
        {
            auto iter = handle_priv->transfers.find(transfer->endpoint);
            if (iter != handle_priv->transfers.end())
            {
                std::lock_guard<std::mutex> lock(iter->second.transfer_mutex);
                if (iter->second.active_transfer)
                {
                    // Still working on a transfer - will get to this transfer later
                    iter->second.transfer_queue.push_back(itransfer);
                    return LIBUSB_SUCCESS;
                }
                else
                {
                    iter->second.active_transfer = itransfer;
                }
            }
            else
            {
                handle_priv->transfers[transfer->endpoint].active_transfer = itransfer;
            }

            return winrt_submit_bulk_transfer(itransfer);
        }
        break;

        case LIBUSB_TRANSFER_TYPE_INTERRUPT:
        {
            // TODO
            return LIBUSB_ERROR_OTHER;
        }
        break;

        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
            usbi_err(TRANSFER_CTX(transfer), "ISOCHRONOUS transfer type not supported by winrt %d");
            return LIBUSB_ERROR_NOT_SUPPORTED;

        default:
            // Should not get here since windows_submit_transfer() validates
            // the transfer->type field
            usbi_err(TRANSFER_CTX(transfer), "unknown endpoint type %d", transfer->type);
            return LIBUSB_ERROR_INVALID_PARAM;
    }
}

static int winrt_pop_transfer_from_queue(usbi_transfer *itransfer, winrt_transfer_queue& queue)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);

    std::unique_lock<std::mutex> lock(queue.transfer_mutex);

    if (queue.active_transfer == itransfer)
    {
        // This was a control transfer that just completed
        winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));
        tpriv->cancel_fn = nullptr;
        queue.active_transfer = nullptr;

        if (queue.transfer_queue.empty())
        {
            // No more transfers to process
            return LIBUSB_SUCCESS;
        }

        // Submit the next transfer and return
        itransfer = queue.transfer_queue.front();
        queue.transfer_queue.pop_front();
        queue.active_transfer = itransfer;
        switch (transfer->type)
        {
            case LIBUSB_TRANSFER_TYPE_CONTROL:
                return winrt_submit_control_transfer(itransfer);

            case LIBUSB_TRANSFER_TYPE_BULK: // Fall through
            case LIBUSB_TRANSFER_TYPE_BULK_STREAM:
                return winrt_submit_bulk_transfer(itransfer);

            case LIBUSB_TRANSFER_TYPE_INTERRUPT:
                // TODO
                return LIBUSB_ERROR_OTHER;

            case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
                return LIBUSB_ERROR_NOT_SUPPORTED;

            default:
                return LIBUSB_ERROR_INVALID_PARAM;
        }
    }

    return LIBUSB_ERROR_NOT_FOUND;
}

static int winrt_pop_transfer(usbi_transfer *itransfer)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(transfer->dev_handle->dev));

    int r = winrt_pop_transfer_from_queue(itransfer, priv->control_transfers);
    if (r != LIBUSB_ERROR_NOT_FOUND)
    {
        return r;
    }

    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(transfer->dev_handle));
    auto iter = handle_priv->transfers.find(transfer->endpoint);
    if (iter != handle_priv->transfers.end())
    {
        return winrt_pop_transfer_from_queue(itransfer, iter->second);
    }

    return LIBUSB_ERROR_NOT_FOUND;
}

static void winrt_transfer_completed(usbi_transfer *itransfer, libusb_transfer_status status)
{
    // Pop transfer and immediately start the next if it's available
    winrt_pop_transfer(itransfer);

    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));
    tpriv->status = status;

    usbi_signal_transfer_completion(itransfer);
}

static int winrt_handle_transfer_completion(struct usbi_transfer *itransfer)
{
    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));

    // Save the status value before destruction
    libusb_transfer_status status = tpriv->status;

    // Explicitly call destructor for private data (if re-used, placement new will be called again later)
    tpriv->~winrt_transfer_priv();

    if (status == LIBUSB_TRANSFER_CANCELLED)
    {
        usbi_handle_transfer_cancellation(itransfer);
    }
    else
    {
        usbi_handle_transfer_completion(itransfer, status);
    }

    return LIBUSB_SUCCESS;
}

static int winrt_cancel_transfer_from_queue(usbi_transfer *itransfer, winrt_transfer_queue& queue)
{
    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));

    std::unique_lock<std::mutex> lock(queue.transfer_mutex);

    if (queue.active_transfer == itransfer)
    {

        if (tpriv->cancel_fn)
        {
            tpriv->cancel_fn();
            // The callback function will complete the transfer once it's fully canceled
            return LIBUSB_SUCCESS;
        }
        else
        {
            // This isn't expected
            // TODO: describe this error
            return LIBUSB_ERROR_OTHER;
        }
    }

    for (auto iter = queue.transfer_queue.begin(); iter != queue.transfer_queue.end(); ++iter)
    {
        if ((*iter) == itransfer)
        {
            queue.transfer_queue.erase(iter);
            lock.unlock();

            winrt_context_priv *pctx = static_cast<winrt_context_priv*>(usbi_get_context_priv(itransfer->dev->ctx));
            tpriv->status = LIBUSB_TRANSFER_CANCELLED;
            usbi_signal_transfer_completion(itransfer);

            return LIBUSB_SUCCESS;
        }
    }

    return LIBUSB_ERROR_NOT_FOUND;
}

static int winrt_cancel_transfer(struct usbi_transfer *itransfer)
{
    libusb_transfer *transfer = USBI_TRANSFER_TO_LIBUSB_TRANSFER(itransfer);
    winrt_transfer_priv *tpriv = static_cast<winrt_transfer_priv*>(usbi_get_transfer_priv(itransfer));
    winrt_device_priv *priv = static_cast<winrt_device_priv*>(usbi_get_device_priv(transfer->dev_handle->dev));

    int r = winrt_cancel_transfer_from_queue(itransfer, priv->control_transfers);
    if (r != LIBUSB_ERROR_NOT_FOUND)
    {
        return r;
    }

    winrt_device_handle_priv *handle_priv = static_cast<winrt_device_handle_priv*>(usbi_get_device_handle_priv(transfer->dev_handle));
    auto iter = handle_priv->transfers.find(transfer->endpoint);
    if (iter != handle_priv->transfers.end())
    {
        return winrt_cancel_transfer_from_queue(itransfer, iter->second);
    }

    return LIBUSB_ERROR_NOT_FOUND;
}

int winrt_handle_events(struct libusb_context *ctx, void *event_data, unsigned int count, unsigned int num_ready)
{
    return LIBUSB_SUCCESS;
}

const usbi_os_backend usbi_backend = {
    "winrt", // name
    USBI_CAP_SUPPORTS_DETACH_KERNEL_DRIVER, // caps
    winrt_init, // init
    winrt_exit, // exit
    winrt_set_option, // set_option
    winrt_get_device_list, // get_device_list
    NULL, // hotplug_poll
    NULL, // wrap_sys_device
    winrt_open, // open
    winrt_close, // close
    winrt_get_active_config_descriptor, // get_active_config_descriptor
    winrt_get_config_descriptor, // get_config_descriptor
    winrt_get_config_descriptor_by_value, // get_config_descriptor_by_value
    winrt_get_configuration, // get_configuration
    winrt_set_configuration, // set_configuration

    winrt_claim_interface, // claim_interface
    winrt_release_interface, // release_interface

    winrt_set_interface_altsetting, // set_interface_altsetting
    winrt_clear_halt, // clear_halt
    winrt_reset_device, // reset_device

    NULL, // alloc_streams
    NULL, // free_streams
    NULL, // dev_mem_alloc
    NULL, // dev_mem_free
    NULL, // kernel_driver_active

    NULL, // detach_kernel_driver
    NULL, // attach_kernel_driver

    winrt_destroy_device, // destroy_device

    winrt_submit_transfer, // submit_transfer
    winrt_cancel_transfer, // cancel_transfer
    NULL, // clear_transfer_priv
    winrt_handle_events, // handle_events
    winrt_handle_transfer_completion, // handle_transfer_completion

    sizeof(winrt_context_priv), // context_priv_size
    sizeof(winrt_device_priv), // device_priv_size
    sizeof(winrt_device_handle_priv), // device_handle_priv_size
    sizeof(winrt_transfer_priv), // transfer_priv_size
};
