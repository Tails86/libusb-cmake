
#ifndef LIBUSB_WINDOWS_WINRT_H
#define LIBUSB_WINDOWS_WINRT_H

#include "libusbi.h"

// Events


// private structures
struct winrt_context_priv
{
	// const struct windows_backend *backend;
	// HANDLE completion_port;
	// HANDLE completion_port_thread;
};

struct winrt_cached_device
{
//   struct list_head      list;
//   IOUSBDeviceDescriptor dev_descriptor;
//   UInt32                location;
//   UInt64                parent_session;
//   UInt64                session;
//   USBDeviceAddress      address;
//   char                  sys_path[21];
//   usb_device_t          device;
//   io_service_t          service;
//   int                   open_count;
//   UInt8                 first_config, active_config, port;
//   int                   can_enumerate;
//   int                   refcount;
//   bool                  in_reenumerate;
//   int                   capture_count;
};

struct winrt_device_priv
{
  winrt_cached_device *dev;
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
