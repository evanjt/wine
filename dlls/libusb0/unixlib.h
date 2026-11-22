/*
 * libusb0 (libusb-win32 compatible) Unix library interface
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_LIBUSB0_UNIXLIB_H
#define __WINE_LIBUSB0_UNIXLIB_H

#include "windef.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* libusb-win32 returns negative errno values from its own error.h */
#define LUSB0_EPERM      1
#define LUSB0_ENOENT     2
#define LUSB0_EIO        5
#define LUSB0_ENOMEM    12
#define LUSB0_EACCES    13
#define LUSB0_EFAULT    14
#define LUSB0_EBUSY     16
#define LUSB0_ENODEV    19
#define LUSB0_EINVAL    22
#define LUSB0_EPIPE     32
#define LUSB0_ENOSYS    38
#define LUSB0_EOVERFLOW 75
#define LUSB0_ETIMEDOUT 116

#define LUSB0_MAX_CONFIG_SIZE 4096

struct lusb0_device_info
{
    UINT8  bus;
    UINT8  address;
    UINT8  device_descriptor[18];
    UINT16 config_length;            /* length of config[], 0 if unavailable */
    UINT8  config[LUSB0_MAX_CONFIG_SIZE];
};

struct lusb0_enumerate_params
{
    struct lusb0_device_info *devices;
    UINT32 max;
    UINT32 count;
};

struct lusb0_open_params
{
    UINT8 bus;
    UINT8 address;
    UINT64 handle;
    int ret;
};

struct lusb0_handle_params
{
    UINT64 handle;
    int value;
    int ret;
};

struct lusb0_string_params
{
    UINT64 handle;
    int index;
    int langid;     /* 0 = ASCII via first language */
    void *buffer;
    UINT32 length;
    int ret;
};

struct lusb0_transfer_params
{
    UINT64 handle;
    UINT8 endpoint;
    void *buffer;
    int length;
    int timeout;
    int ret;
};

struct lusb0_control_params
{
    UINT64 handle;
    int request_type;
    int request;
    int value;
    int index;
    void *buffer;
    int length;
    int timeout;
    int ret;
};

struct lusb0_async_submit_params
{
    UINT64 handle;
    UINT64 context;   /* in: existing context or 0, out: context */
    UINT8 endpoint;
    UINT8 type;       /* LUSB0_ASYNC_BULK or LUSB0_ASYNC_INTERRUPT */
    void *buffer;
    int length;
    int ret;
};

#define LUSB0_ASYNC_BULK      0
#define LUSB0_ASYNC_INTERRUPT 1

struct lusb0_async_params
{
    UINT64 context;
    int timeout;
    int cancel;      /* reap: cancel on timeout */
    int ret;
};

enum lusb0_funcs
{
    unix_lusb0_init,
    unix_lusb0_enumerate,
    unix_lusb0_open,
    unix_lusb0_close,
    unix_lusb0_set_configuration,
    unix_lusb0_claim_interface,
    unix_lusb0_release_interface,
    unix_lusb0_set_altinterface,
    unix_lusb0_clear_halt,
    unix_lusb0_reset,
    unix_lusb0_get_string,
    unix_lusb0_bulk_transfer,
    unix_lusb0_control_transfer,
    unix_lusb0_async_submit,
    unix_lusb0_async_reap,
    unix_lusb0_async_cancel,
    unix_lusb0_async_free,
    unix_lusb0_funcs_count,
};

#endif /* __WINE_LIBUSB0_UNIXLIB_H */
