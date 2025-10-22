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
	winrt::Windows::Devices::Usb::UsbDevice default_device;
	//! Maps interface numbers to claimed interfaces
	std::unordered_map<uint8_t, winrt::Windows::Devices::Usb::UsbDevice> claimed_interfaces;
};

struct winrt_interface
{
//  usb_interface_t      interface;
//   uint8_t              num_endpoints;
//   CFRunLoopSourceRef   cfSource;
//   uint64_t             frames[256];
//   uint8_t              endpoint_addrs[USB_MAXENDPOINTS];
};

struct winrt_device_handle_priv
{
//   bool                 is_open;
//   CFRunLoopSourceRef   cfSource;

   winrt_interface interfaces[USB_MAXINTERFACES];
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
