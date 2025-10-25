/*
 * windows hotplug backend for libusb 1.0
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

// Events


// private structures
struct winrt_context_priv
{
    std::list<usbi_transfer*> canceled_floating_transfers;
};

struct winrt_transfer_queue
{
    // Currently processing transfer
    usbi_transfer* active_transfer;
    // Queue of transfers in progress
    std::list<usbi_transfer*> transfer_queue;
    // Mutex serializing access to the above transfer data
    std::mutex transfer_mutex;
};

struct winrt_device_priv
{
    //! The active configuration
    uint8_t active_config = 1;

    //! Stores each configuration descriptor once opened
    std::vector<std::vector<uint8_t>> config_descriptors;

    //! String representation of System.Devices.ContainerId for this device
    std::wstring container_id;
    // Because of the way winrt is setup, a UsbDevice must be claimed to perform any operation
    winrt::Windows::Devices::Usb::UsbDevice default_device = nullptr;
    // The ID of the default device
    std::wstring default_device_id;

    // Keeps track of all current control transfers
    winrt_transfer_queue control_transfers;
};

struct winrt_interface
{
    winrt::Windows::Devices::Usb::UsbDevice device;
    std::wstring device_id;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkInPipe> bulk_in_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkOutPipe> bulk_out_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptInPipe> interrupt_in_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptOutPipe> interrupt_out_pipes;
};

struct winrt_device_handle_priv
{
    std::unordered_map<uint8_t, winrt_interface> interfaces;
    std::unordered_map<uint8_t, winrt_transfer_queue> transfers;
};

struct winrt_transfer_priv
{
    // The function to call in order to cancel the asynchronous communication operation
    std::function<void()> cancel_fn;
};

#endif // LIBUSB_WINDOWS_WINRT_H
