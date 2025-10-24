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
#include <winrt/base.h>
#include <winrt/Windows.Devices.Usb.h>

// Events


// private structures
struct winrt_context_priv
{
    // Nothing needed here yet
};

struct winrt_device_priv
{
    //! String representation of System.Devices.ContainerId for this device
    std::wstring container_id;
    // Because of the way winrt is setup, a UsbDevice must be claimed to perform any operation
    winrt::Windows::Devices::Usb::UsbDevice default_device = nullptr;

    // Queue of control transfers in progress (top of queue is currently active one)
    std::list<usbi_transfer*> control_transfer_queue;
    // Mutex serializing the above queue
    std::mutex control_transfer_queue_mutex;
};

struct winrt_interface
{
    winrt::Windows::Devices::Usb::UsbDevice device;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkInPipe> bulk_in_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbBulkOutPipe> bulk_out_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptInPipe> interrupt_in_pipes;
    std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbInterruptOutPipe> interrupt_out_pipes;
};

struct winrt_device_handle_priv
{
    std::unordered_map<uint8_t, winrt_interface> interfaces;
};

struct winrt_transfer_priv
{
//   /* Isoc */
//   IOUSBIsocFrame *isoc_framelist;
//   int num_iso_packets;

//   /* Control */
//   IOUSBDevRequestTO req;

//   /* Bulk */

//   /* Completion status */
//   IOReturn result;
//   UInt32 size;
};

#endif // LIBUSB_WINDOWS_WINRT_H
