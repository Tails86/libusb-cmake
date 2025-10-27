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

#ifndef LIBUSB_WINDOWS_WINRT_H
#define LIBUSB_WINDOWS_WINRT_H

#include "libusbi.h"

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <vector>
#include <functional>
#include <winrt/base.h>
#include <winrt/Windows.Devices.Usb.h>

//
// private structures
//

struct winrt_context_priv
{
    //! Mutex serializing access to container_id_to_session_id_map and last_session_id
    std::mutex container_id_to_session_id_mutex;
    //! Maps container ID to a unique session ID
    std::unordered_map<winrt::guid, unsigned long> container_id_to_session_id_map;
    //! The last used session ID
    unsigned long last_session_id = 0;
};

struct winrt_transfer_queue
{
    //! Currently processing transfer
    usbi_transfer* active_transfer;
    //! Queue of transfers waiting to be processed
    std::list<usbi_transfer*> transfer_queue;
    //! Mutex serializing access to the above transfer data
    std::mutex transfer_mutex;
};

struct winrt_device_priv
{
    //! The active configuration (0 if not retrieved yet)
    uint8_t active_config = 0;

    //! Stores each configuration descriptor (only filled once any descriptor is requested)
    std::vector<std::vector<uint8_t>> config_descriptors;

    //! String representation of System.Devices.ContainerId for this device
    std::wstring container_id;
    //! Because of the way winrt is setup, a UsbDevice must be claimed to perform any operation like control transfers
    winrt::Windows::Devices::Usb::UsbDevice default_device = nullptr;
    //! The ID of the default device
    std::wstring default_device_id;

    //! Keeps track of all current control transfers
    winrt_transfer_queue control_transfers;
};

struct winrt_interface
{
    //! The device that this interface is associated with
    winrt::Windows::Devices::Usb::UsbDevice device;
    //! The device ID used to open the above device
    std::wstring device_id;
    //! Maps endpoint number to bulk input pipe
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkInPipe> bulk_in_pipes;
    //! Maps endpoint number to bulk output pipe
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkOutPipe> bulk_out_pipes;
    //! Maps endpoint number to interrupt input pipe
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptInPipe> interrupt_in_pipes;
    //! Maps endpoint number to interrupt output pipe
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptOutPipe> interrupt_out_pipes;
};

struct winrt_device_handle_priv
{
    //! Maps interface number to winrt_interface
    std::unordered_map<uint8_t, winrt_interface> interfaces;
    //! Maps endpoint number to winrt_transfer_queue
    std::unordered_map<uint8_t, winrt_transfer_queue> transfers;
};

struct winrt_transfer_priv
{
    //! The status of this transfer once completed by winrt operation
    libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
    //! The function to call in order to cancel the asynchronous communication operation
    std::function<void()> cancel_fn;
};

#endif // LIBUSB_WINDOWS_WINRT_H
