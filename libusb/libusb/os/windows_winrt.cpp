#include "libusbi.h"
#include "windows_winrt.hpp" // TODO: remove

#include <unordered_map>
#include <string>
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
//! @param[in] getFn A function executing the async get
//! @return The retrieved value
template <typename T>
static T winrt_async_get(const std::function<T()>& getFn)
{
    try
    {
        if (winrt::impl::is_sta_thread())
        {
            // To avoid assertion check, run within another thread
            std::future<T> task = std::async(std::launch::async, getFn);
            return task.get();
        }
        else
        {
            return getFn();
        }
    }
    catch(const winrt::hresult_error&)
    {
        // Execution error occurred
        return nullptr;
    }
}

static int winrt_init(struct libusb_context *ctx)
{
    UNUSED(ctx);
    // winrt_context_priv *priv = static_cast<winrt_context_priv*>(usbi_get_context_priv(ctx));
    // TODO
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
                L" AND System.Devices.DeviceInstanceId:~<\"USB\\\"",
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

                // TODO
                // winrt_device_priv *dpriv = static_cast<winrt_device_priv*>(usbi_get_device_priv(dev));

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

                winrt::Windows::Devices::Usb::UsbDeviceDescriptor winrtDesc = winrtDev.DeviceDescriptor();

                libusb_device_descriptor& device_descriptor = dev->device_descriptor;
                device_descriptor.bLength = LIBUSB_DT_DEVICE_SIZE;
                device_descriptor.bDescriptorType = LIBUSB_DT_DEVICE;
                device_descriptor.bcdUSB = static_cast<uint16_t>(winrtDesc.BcdUsb());
                device_descriptor.bMaxPacketSize0 = winrtDesc.MaxPacketSize0();
                device_descriptor.idVendor = winrtDesc.VendorId();
                device_descriptor.idProduct = winrtDesc.ProductId();
                device_descriptor.bcdDevice = static_cast<uint16_t>(winrtDesc.BcdDeviceRevision());
                device_descriptor.iManufacturer = SIMULATED_IMANUFACTURER;
                device_descriptor.iProduct = SIMULATED_IPRODUCT;
                device_descriptor.iSerialNumber = SIMULATED_ISERIAL;
                device_descriptor.bNumConfigurations = winrtDesc.NumberOfConfigurations();

                int err = usbi_sanitize_device(dev);
                if (err) {
                    libusb_unref_device(dev);
                    return err;
                }
            }

            if (discovered_devs_append(*_discdevs, dev) == NULL)
                return LIBUSB_ERROR_NO_MEM;

            libusb_unref_device(dev);

            foundContainerIds.insert(containerId);
        }
    }

	return LIBUSB_SUCCESS;
}

static int winrt_open(struct libusb_device_handle *dev_handle)
{
    return LIBUSB_ERROR_IO;
}

static void winrt_close(struct libusb_device_handle *dev_handle)
{
}

static int winrt_get_active_config_descriptor(struct libusb_device *dev, void *buffer, size_t len)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_get_config_descriptor(struct libusb_device *dev, uint8_t config_index, void *buffer, size_t len)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_get_config_descriptor_by_value(
    struct libusb_device *dev,
	uint8_t bConfigurationValue,
    void **buffer
)
{
	return LIBUSB_ERROR_IO;
}

static int winrt_get_configuration(struct libusb_device_handle *dev_handle, uint8_t *config)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_set_configuration(struct libusb_device_handle *dev_handle, int config)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_claim_interface(struct libusb_device_handle *dev_handle, uint8_t iface)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_release_interface(struct libusb_device_handle *dev_handle, uint8_t iface)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_set_interface_altsetting(struct libusb_device_handle *dev_handle, uint8_t iface, uint8_t altsetting)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_clear_halt(struct libusb_device_handle *dev_handle, unsigned char endpoint)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_reset_device(struct libusb_device_handle *dev_handle)
{
    return LIBUSB_ERROR_IO;
}

static void winrt_destroy_device(struct libusb_device *dev)
{
}

static int winrt_submit_transfer(struct usbi_transfer *itransfer)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_cancel_transfer(struct usbi_transfer *itransfer)
{
    return LIBUSB_ERROR_IO;
}

static int winrt_handle_transfer_completion(struct usbi_transfer *itransfer)
{
    return LIBUSB_ERROR_IO;
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
    NULL, // handle_events
    winrt_handle_transfer_completion, // handle_transfer_completion

    sizeof(winrt_context_priv), // context_priv_size
    sizeof(winrt_device_priv), // device_priv_size
    sizeof(winrt_device_handle_priv), // device_handle_priv_size
    sizeof(winrt_transfer_priv), // transfer_priv_size
};
