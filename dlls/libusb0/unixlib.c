/*
 * libusb0 (libusb-win32 compatible) Unix library, backed by libusb-1.0
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

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libusb.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(libusb0);

static libusb_context *ctx;

struct async_context
{
    struct libusb_transfer *transfer;
    libusb_device_handle *handle;
    UINT8 endpoint;
    UINT8 type;
    int submitted;   /* a transfer is in flight */
    int completed;   /* set by the libusb callback */
};

static int map_error( int err )
{
    switch (err)
    {
    case LIBUSB_SUCCESS:             return 0;
    case LIBUSB_ERROR_IO:            return -LUSB0_EIO;
    case LIBUSB_ERROR_INVALID_PARAM: return -LUSB0_EINVAL;
    case LIBUSB_ERROR_ACCESS:        return -LUSB0_EACCES;
    case LIBUSB_ERROR_NO_DEVICE:     return -LUSB0_ENODEV;
    case LIBUSB_ERROR_NOT_FOUND:     return -LUSB0_ENOENT;
    case LIBUSB_ERROR_BUSY:          return -LUSB0_EBUSY;
    case LIBUSB_ERROR_TIMEOUT:       return -LUSB0_ETIMEDOUT;
    case LIBUSB_ERROR_OVERFLOW:      return -LUSB0_EOVERFLOW;
    case LIBUSB_ERROR_PIPE:          return -LUSB0_EPIPE;
    case LIBUSB_ERROR_NO_MEM:        return -LUSB0_ENOMEM;
    case LIBUSB_ERROR_NOT_SUPPORTED: return -LUSB0_ENOSYS;
    default:                         return -LUSB0_EIO;
    }
}

static int map_transfer_status( enum libusb_transfer_status status )
{
    switch (status)
    {
    case LIBUSB_TRANSFER_COMPLETED: return 0;
    case LIBUSB_TRANSFER_TIMED_OUT: return -LUSB0_ETIMEDOUT;
    case LIBUSB_TRANSFER_STALL:     return -LUSB0_EPIPE;
    case LIBUSB_TRANSFER_NO_DEVICE: return -LUSB0_ENODEV;
    case LIBUSB_TRANSFER_OVERFLOW:  return -LUSB0_EOVERFLOW;
    case LIBUSB_TRANSFER_CANCELLED:
    case LIBUSB_TRANSFER_ERROR:
    default:                        return -LUSB0_EIO;
    }
}

static NTSTATUS lusb0_init( void *args )
{
    int ret;

    if (ctx) return STATUS_SUCCESS;
    if ((ret = libusb_init( &ctx )))
    {
        ERR( "Failed to initialize libusb: %s\n", libusb_strerror( ret ));
        ctx = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

/* Re-encode a parsed configuration descriptor into its raw wire format. */
static UINT16 encode_config( const struct libusb_config_descriptor *config, UINT8 *out, size_t size )
{
    size_t pos = 0;
    int i, j, k;

#define PUT(b) do { if (pos >= size) return 0; out[pos++] = (b); } while (0)
#define PUT_EXTRA(p, n) do { if (pos + (n) > size) return 0; memcpy( out + pos, (p), (n) ); pos += (n); } while (0)

    PUT( 9 ); PUT( LIBUSB_DT_CONFIG ); PUT( 0 ); PUT( 0 ); /* total length patched below */
    PUT( config->bNumInterfaces ); PUT( config->bConfigurationValue );
    PUT( config->iConfiguration ); PUT( config->bmAttributes ); PUT( config->MaxPower );
    if (config->extra_length) PUT_EXTRA( config->extra, config->extra_length );

    for (i = 0; i < config->bNumInterfaces; ++i)
    {
        const struct libusb_interface *iface = &config->interface[i];

        for (j = 0; j < iface->num_altsetting; ++j)
        {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];

            PUT( 9 ); PUT( LIBUSB_DT_INTERFACE ); PUT( alt->bInterfaceNumber );
            PUT( alt->bAlternateSetting ); PUT( alt->bNumEndpoints ); PUT( alt->bInterfaceClass );
            PUT( alt->bInterfaceSubClass ); PUT( alt->bInterfaceProtocol ); PUT( alt->iInterface );
            if (alt->extra_length) PUT_EXTRA( alt->extra, alt->extra_length );

            for (k = 0; k < alt->bNumEndpoints; ++k)
            {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[k];

                PUT( 7 ); PUT( LIBUSB_DT_ENDPOINT ); PUT( ep->bEndpointAddress );
                PUT( ep->bmAttributes ); PUT( ep->wMaxPacketSize & 0xff ); PUT( ep->wMaxPacketSize >> 8 );
                PUT( ep->bInterval );
                if (ep->extra_length) PUT_EXTRA( ep->extra, ep->extra_length );
            }
        }
    }
#undef PUT
#undef PUT_EXTRA

    out[2] = pos & 0xff;
    out[3] = pos >> 8;
    return pos;
}

static NTSTATUS lusb0_enumerate( void *args )
{
    struct lusb0_enumerate_params *params = args;
    libusb_device **list;
    ssize_t count, i;

    params->count = 0;
    if (!ctx) return STATUS_UNSUCCESSFUL;

    if ((count = libusb_get_device_list( ctx, &list )) < 0)
    {
        ERR( "Failed to get device list: %s\n", libusb_strerror( count ));
        return STATUS_UNSUCCESSFUL;
    }

    for (i = 0; i < count && params->count < params->max; ++i)
    {
        struct lusb0_device_info *info = &params->devices[params->count];
        struct libusb_device_descriptor desc;
        struct libusb_config_descriptor *config;
        UINT8 *d = info->device_descriptor;

        if (libusb_get_device_descriptor( list[i], &desc )) continue;

        info->bus = libusb_get_bus_number( list[i] );
        info->address = libusb_get_device_address( list[i] );

        d[0] = LIBUSB_DT_DEVICE_SIZE; d[1] = LIBUSB_DT_DEVICE;
        d[2] = desc.bcdUSB & 0xff; d[3] = desc.bcdUSB >> 8;
        d[4] = desc.bDeviceClass; d[5] = desc.bDeviceSubClass; d[6] = desc.bDeviceProtocol;
        d[7] = desc.bMaxPacketSize0;
        d[8] = desc.idVendor & 0xff; d[9] = desc.idVendor >> 8;
        d[10] = desc.idProduct & 0xff; d[11] = desc.idProduct >> 8;
        d[12] = desc.bcdDevice & 0xff; d[13] = desc.bcdDevice >> 8;
        d[14] = desc.iManufacturer; d[15] = desc.iProduct; d[16] = desc.iSerialNumber;
        d[17] = desc.bNumConfigurations;

        info->config_length = 0;
        if (desc.bNumConfigurations && !libusb_get_config_descriptor( list[i], 0, &config ))
        {
            info->config_length = encode_config( config, info->config, sizeof(info->config) );
            libusb_free_config_descriptor( config );
        }

        TRACE( "bus %u address %u: %04x:%04x\n", info->bus, info->address, desc.idVendor, desc.idProduct );
        params->count++;
    }

    libusb_free_device_list( list, 1 );
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_open( void *args )
{
    struct lusb0_open_params *params = args;
    libusb_device **list;
    libusb_device_handle *handle = NULL;
    ssize_t count, i;
    int ret = LIBUSB_ERROR_NOT_FOUND;

    params->handle = 0;
    if (!ctx) { params->ret = -LUSB0_ENODEV; return STATUS_SUCCESS; }

    if ((count = libusb_get_device_list( ctx, &list )) < 0)
    {
        params->ret = map_error( count );
        return STATUS_SUCCESS;
    }

    for (i = 0; i < count; ++i)
    {
        if (libusb_get_bus_number( list[i] ) != params->bus) continue;
        if (libusb_get_device_address( list[i] ) != params->address) continue;
        ret = libusb_open( list[i], &handle );
        break;
    }
    libusb_free_device_list( list, 1 );

    if (ret)
    {
        WARN( "Failed to open device %u:%u: %s\n", params->bus, params->address, libusb_strerror( ret ));
        params->ret = map_error( ret );
        return STATUS_SUCCESS;
    }

    /* Let libusb detach Linux kernel drivers (e.g. usb_serial_simple) when claiming. */
    libusb_set_auto_detach_kernel_driver( handle, 1 );

    params->handle = (UINT_PTR)handle;
    params->ret = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_close( void *args )
{
    struct lusb0_handle_params *params = args;

    libusb_close( (libusb_device_handle *)(UINT_PTR)params->handle );
    params->ret = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_set_configuration( void *args )
{
    struct lusb0_handle_params *params = args;
    libusb_device_handle *handle = (libusb_device_handle *)(UINT_PTR)params->handle;
    int current = -1, ret;

    /* Re-selecting the active configuration fails with EBUSY while a kernel
     * driver is bound, and is a no-op anyway. */
    if (!libusb_get_configuration( handle, &current ) && current == params->value)
    {
        TRACE( "configuration %d already active\n", current );
        params->ret = 0;
        return STATUS_SUCCESS;
    }

    ret = libusb_set_configuration( handle, params->value );
    if (ret) WARN( "set_configuration(%d) failed: %s\n", params->value, libusb_strerror( ret ));
    params->ret = map_error( ret );
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_claim_interface( void *args )
{
    struct lusb0_handle_params *params = args;
    int ret = libusb_claim_interface( (libusb_device_handle *)(UINT_PTR)params->handle, params->value );

    if (ret) WARN( "claim_interface(%d) failed: %s\n", params->value, libusb_strerror( ret ));
    params->ret = map_error( ret );
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_release_interface( void *args )
{
    struct lusb0_handle_params *params = args;

    params->ret = map_error( libusb_release_interface( (libusb_device_handle *)(UINT_PTR)params->handle,
                                                       params->value ));
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_set_altinterface( void *args )
{
    struct lusb0_handle_params *params = args;
    libusb_device_handle *handle = (libusb_device_handle *)(UINT_PTR)params->handle;

    /* params->value packs interface << 8 | alternate setting */
    params->ret = map_error( libusb_set_interface_alt_setting( handle, params->value >> 8, params->value & 0xff ));
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_clear_halt( void *args )
{
    struct lusb0_handle_params *params = args;

    params->ret = map_error( libusb_clear_halt( (libusb_device_handle *)(UINT_PTR)params->handle,
                                                params->value ));
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_reset( void *args )
{
    struct lusb0_handle_params *params = args;

    params->ret = map_error( libusb_reset_device( (libusb_device_handle *)(UINT_PTR)params->handle ));
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_get_string( void *args )
{
    struct lusb0_string_params *params = args;
    libusb_device_handle *handle = (libusb_device_handle *)(UINT_PTR)params->handle;
    int ret;

    if (params->langid)
        ret = libusb_get_string_descriptor( handle, params->index, params->langid,
                                            params->buffer, params->length );
    else
        ret = libusb_get_string_descriptor_ascii( handle, params->index, params->buffer, params->length );

    params->ret = ret < 0 ? map_error( ret ) : ret;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_bulk_transfer( void *args )
{
    struct lusb0_transfer_params *params = args;
    int transferred = 0, ret;

    /* Linux usbfs converts bulk requests on interrupt endpoints, but rejects
     * interrupt requests on bulk endpoints, so always use bulk here. */
    ret = libusb_bulk_transfer( (libusb_device_handle *)(UINT_PTR)params->handle, params->endpoint,
                                params->buffer, params->length, &transferred, params->timeout );

    if (ret && !(ret == LIBUSB_ERROR_TIMEOUT && transferred))
        params->ret = map_error( ret );
    else
        params->ret = transferred;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_control_transfer( void *args )
{
    struct lusb0_control_params *params = args;
    int ret;

    ret = libusb_control_transfer( (libusb_device_handle *)(UINT_PTR)params->handle, params->request_type,
                                   params->request, params->value, params->index,
                                   params->buffer, params->length, params->timeout );
    params->ret = ret < 0 ? map_error( ret ) : ret;
    return STATUS_SUCCESS;
}

static void LIBUSB_CALL async_callback( struct libusb_transfer *transfer )
{
    struct async_context *async = transfer->user_data;

    async->completed = 1;
}

static NTSTATUS lusb0_async_submit( void *args )
{
    struct lusb0_async_submit_params *params = args;
    struct async_context *async = (struct async_context *)(UINT_PTR)params->context;
    int ret;

    if (!async)
    {
        if (!(async = calloc( 1, sizeof(*async) )) || !(async->transfer = libusb_alloc_transfer( 0 )))
        {
            free( async );
            params->ret = -LUSB0_ENOMEM;
            return STATUS_SUCCESS;
        }
        async->handle = (libusb_device_handle *)(UINT_PTR)params->handle;
        async->endpoint = params->endpoint;
        async->type = params->type;
        params->context = (UINT_PTR)async;
        params->ret = 0;
        return STATUS_SUCCESS;
    }

    if (async->submitted)
    {
        params->ret = -LUSB0_EBUSY;
        return STATUS_SUCCESS;
    }

    /* See lusb0_bulk_transfer: bulk type works on both bulk and interrupt endpoints. */
    libusb_fill_bulk_transfer( async->transfer, async->handle, async->endpoint,
                               params->buffer, params->length, async_callback, async, 0 );
    async->completed = 0;
    if ((ret = libusb_submit_transfer( async->transfer )))
    {
        WARN( "submit on endpoint %#x failed: %s\n", async->endpoint, libusb_strerror( ret ));
        params->ret = map_error( ret );
        return STATUS_SUCCESS;
    }
    async->submitted = 1;
    params->ret = 0;
    return STATUS_SUCCESS;
}

/* Process libusb events until the transfer completes or timeout_ms passes (-1 = forever). */
static void wait_for_completion( struct async_context *async, int timeout_ms )
{
    struct timespec start, now;
    struct timeval tv;
    long elapsed;

    clock_gettime( CLOCK_MONOTONIC, &start );
    while (!async->completed)
    {
        if (timeout_ms >= 0)
        {
            clock_gettime( CLOCK_MONOTONIC, &now );
            elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
            if (elapsed >= timeout_ms) break;
            tv.tv_sec = (timeout_ms - elapsed) / 1000;
            tv.tv_usec = ((timeout_ms - elapsed) % 1000) * 1000;
        }
        else
        {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }
        libusb_handle_events_timeout_completed( ctx, &tv, &async->completed );
    }
}

static NTSTATUS lusb0_async_reap( void *args )
{
    struct lusb0_async_params *params = args;
    struct async_context *async = (struct async_context *)(UINT_PTR)params->context;
    int ret;

    if (!async || !async->submitted)
    {
        params->ret = -LUSB0_EINVAL;
        return STATUS_SUCCESS;
    }

    wait_for_completion( async, params->timeout );

    if (!async->completed)
    {
        if (params->cancel)
        {
            libusb_cancel_transfer( async->transfer );
            wait_for_completion( async, 1000 );
            if (async->completed) async->submitted = 0;
        }
        params->ret = -LUSB0_ETIMEDOUT;
        return STATUS_SUCCESS;
    }

    async->submitted = 0;
    ret = map_transfer_status( async->transfer->status );
    params->ret = ret ? ret : async->transfer->actual_length;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_async_cancel( void *args )
{
    struct lusb0_async_params *params = args;
    struct async_context *async = (struct async_context *)(UINT_PTR)params->context;

    params->ret = 0;
    if (!async || !async->submitted) return STATUS_SUCCESS;

    libusb_cancel_transfer( async->transfer );
    wait_for_completion( async, 1000 );
    if (async->completed) async->submitted = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS lusb0_async_free( void *args )
{
    struct lusb0_async_params *params = args;
    struct async_context *async = (struct async_context *)(UINT_PTR)params->context;

    params->ret = 0;
    if (!async) return STATUS_SUCCESS;

    if (async->submitted)
    {
        libusb_cancel_transfer( async->transfer );
        wait_for_completion( async, 1000 );
        if (!async->completed)
        {
            /* Never free a transfer libusb still owns. */
            ERR( "transfer on endpoint %#x did not finish, leaking it\n", async->endpoint );
            return STATUS_SUCCESS;
        }
    }
    libusb_free_transfer( async->transfer );
    free( async );
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    lusb0_init,
    lusb0_enumerate,
    lusb0_open,
    lusb0_close,
    lusb0_set_configuration,
    lusb0_claim_interface,
    lusb0_release_interface,
    lusb0_set_altinterface,
    lusb0_clear_halt,
    lusb0_reset,
    lusb0_get_string,
    lusb0_bulk_transfer,
    lusb0_control_transfer,
    lusb0_async_submit,
    lusb0_async_reap,
    lusb0_async_cancel,
    lusb0_async_free,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_lusb0_funcs_count );
