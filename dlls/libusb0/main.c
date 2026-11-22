/*
 * libusb0.dll - libusb-win32 compatible API on top of the host libusb
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(libusb0);

/* Structures as laid out by libusb-win32's lusb0_usb.h (descriptors are byte-packed). */
#define LIBUSB_PATH_MAX 512

#include "pshpack1.h"

struct usb_endpoint_descriptor
{
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    UCHAR  bEndpointAddress;
    UCHAR  bmAttributes;
    USHORT wMaxPacketSize;
    UCHAR  bInterval;
    UCHAR  bRefresh;
    UCHAR  bSynchAddress;
    UCHAR *extra;
    int    extralen;
};

struct usb_interface_descriptor
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bInterfaceNumber;
    UCHAR bAlternateSetting;
    UCHAR bNumEndpoints;
    UCHAR bInterfaceClass;
    UCHAR bInterfaceSubClass;
    UCHAR bInterfaceProtocol;
    UCHAR iInterface;
    struct usb_endpoint_descriptor *endpoint;
    UCHAR *extra;
    int    extralen;
};

struct usb_interface
{
    struct usb_interface_descriptor *altsetting;
    int num_altsetting;
};

struct usb_config_descriptor
{
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    USHORT wTotalLength;
    UCHAR  bNumInterfaces;
    UCHAR  bConfigurationValue;
    UCHAR  iConfiguration;
    UCHAR  bmAttributes;
    UCHAR  MaxPower;
    struct usb_interface *iface;
    UCHAR *extra;
    int    extralen;
};

struct usb_device_descriptor
{
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    USHORT bcdUSB;
    UCHAR  bDeviceClass;
    UCHAR  bDeviceSubClass;
    UCHAR  bDeviceProtocol;
    UCHAR  bMaxPacketSize0;
    USHORT idVendor;
    USHORT idProduct;
    USHORT bcdDevice;
    UCHAR  iManufacturer;
    UCHAR  iProduct;
    UCHAR  iSerialNumber;
    UCHAR  bNumConfigurations;
};

#include "poppack.h"

struct usb_bus;

struct usb_device
{
    struct usb_device *next, *prev;
    char filename[LIBUSB_PATH_MAX];
    struct usb_bus *bus;
    struct usb_device_descriptor descriptor;
    struct usb_config_descriptor *config;
    void *dev;
    UCHAR devnum;
    UCHAR num_children;
    struct usb_device **children;
};

struct usb_bus
{
    struct usb_bus *next, *prev;
    char dirname[LIBUSB_PATH_MAX];
    struct usb_device *devices;
    ULONG location;
    struct usb_device *root_dev;
};

typedef struct usb_dev_handle
{
    UINT64 unix_handle;
    struct usb_device *device;
    int interface;
} usb_dev_handle;

struct async_context
{
    usb_dev_handle *dev;
    UINT64 unix_context;
};

static struct usb_bus *busses;
static BOOL unix_ok;
static int debug_level;
static char last_error[256];

static CRITICAL_SECTION lusb0_cs;
static CRITICAL_SECTION_DEBUG lusb0_cs_debug =
{
    0, 0, &lusb0_cs,
    { &lusb0_cs_debug.ProcessLocksList, &lusb0_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": lusb0_cs") }
};
static CRITICAL_SECTION lusb0_cs = { &lusb0_cs_debug, -1, 0, 0, 0, 0 };

static int set_error( int ret, const char *func )
{
    if (ret < 0)
    {
        snprintf( last_error, sizeof(last_error), "libusb0-dll:err [%s] error %d", func, ret );
        if (debug_level) WARN( "%s\n", last_error );
    }
    return ret;
}

/* ---- descriptor parsing ---------------------------------------------------- */

static UCHAR *copy_extra( const UCHAR *data, int len )
{
    UCHAR *extra;

    if (len <= 0 || !(extra = malloc( len ))) return NULL;
    memcpy( extra, data, len );
    return extra;
}

/* Collect class/vendor descriptors following a standard descriptor. */
static int extra_length( const UCHAR *p, const UCHAR *end )
{
    const UCHAR *start = p;

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end &&
           p[1] != 0x04 /* INTERFACE */ && p[1] != 0x05 /* ENDPOINT */ && p[1] != 0x02 /* CONFIG */)
        p += p[0];
    return p - start;
}

static void free_config( struct usb_config_descriptor *config )
{
    int i, j, k;

    if (!config) return;
    for (i = 0; config->iface && i < config->bNumInterfaces; ++i)
    {
        struct usb_interface *iface = &config->iface[i];

        for (j = 0; iface->altsetting && j < iface->num_altsetting; ++j)
        {
            struct usb_interface_descriptor *alt = &iface->altsetting[j];

            for (k = 0; alt->endpoint && k < alt->bNumEndpoints; ++k)
                free( alt->endpoint[k].extra );
            free( alt->endpoint );
            free( alt->extra );
        }
        free( iface->altsetting );
    }
    free( config->iface );
    free( config->extra );
    free( config );
}

static struct usb_config_descriptor *parse_config( const UCHAR *data, unsigned int size )
{
    const UCHAR *p = data, *end = data + size;
    struct usb_config_descriptor *config;
    int len;

    if (size < 9 || data[1] != 0x02) return NULL;
    if (!(config = calloc( 1, sizeof(*config) ))) return NULL;

    config->bLength = 9;
    config->bDescriptorType = 0x02;
    config->wTotalLength = data[2] | (data[3] << 8);
    config->bNumInterfaces = data[4];
    config->bConfigurationValue = data[5];
    config->iConfiguration = data[6];
    config->bmAttributes = data[7];
    config->MaxPower = data[8];
    p += data[0];

    len = extra_length( p, end );
    config->extra = copy_extra( p, len );
    config->extralen = config->extra ? len : 0;
    p += len;

    if (!(config->iface = calloc( config->bNumInterfaces ? config->bNumInterfaces : 1, sizeof(*config->iface) )))
        goto fail;

    while (p + 9 <= end && p[0] >= 9 && p[1] == 0x04)
    {
        struct usb_interface *iface;
        struct usb_interface_descriptor *alt, *alts;
        int k;

        if (p[2] >= config->bNumInterfaces) break;
        iface = &config->iface[p[2]];
        if (!(alts = realloc( iface->altsetting, (iface->num_altsetting + 1) * sizeof(*alts) ))) goto fail;
        iface->altsetting = alts;
        alt = &alts[iface->num_altsetting++];
        memset( alt, 0, sizeof(*alt) );

        alt->bLength = 9;
        alt->bDescriptorType = 0x04;
        alt->bInterfaceNumber = p[2];
        alt->bAlternateSetting = p[3];
        alt->bNumEndpoints = p[4];
        alt->bInterfaceClass = p[5];
        alt->bInterfaceSubClass = p[6];
        alt->bInterfaceProtocol = p[7];
        alt->iInterface = p[8];
        p += p[0];

        len = extra_length( p, end );
        alt->extra = copy_extra( p, len );
        alt->extralen = alt->extra ? len : 0;
        p += len;

        if (alt->bNumEndpoints && !(alt->endpoint = calloc( alt->bNumEndpoints, sizeof(*alt->endpoint) )))
            goto fail;

        for (k = 0; k < alt->bNumEndpoints; ++k)
        {
            struct usb_endpoint_descriptor *ep = &alt->endpoint[k];

            if (p + 7 > end || p[0] < 7 || p[1] != 0x05) { alt->bNumEndpoints = k; break; }
            ep->bLength = p[0];
            ep->bDescriptorType = 0x05;
            ep->bEndpointAddress = p[2];
            ep->bmAttributes = p[3];
            ep->wMaxPacketSize = p[4] | (p[5] << 8);
            ep->bInterval = p[6];
            if (p[0] >= 9) { ep->bRefresh = p[7]; ep->bSynchAddress = p[8]; }
            p += p[0];

            len = extra_length( p, end );
            ep->extra = copy_extra( p, len );
            ep->extralen = ep->extra ? len : 0;
            p += len;
        }
    }
    return config;

fail:
    free_config( config );
    return NULL;
}

/* ---- device list management ---------------------------------------------- */

static struct usb_bus *get_bus( UINT8 busnum, int *changes )
{
    struct usb_bus *bus, *last = NULL;

    for (bus = busses; bus; bus = bus->next)
    {
        if (bus->location == busnum) return bus;
        last = bus;
    }

    if (!(bus = calloc( 1, sizeof(*bus) ))) return NULL;
    snprintf( bus->dirname, sizeof(bus->dirname), "bus-%u", busnum );
    bus->location = busnum;
    bus->prev = last;
    if (last) last->next = bus;
    else busses = bus;
    if (changes) (*changes)++;
    return bus;
}

static void unlink_device( struct usb_device *dev )
{
    if (dev->prev) dev->prev->next = dev->next;
    else dev->bus->devices = dev->next;
    if (dev->next) dev->next->prev = dev->prev;
}

static int refresh_devices( BOOL busses_only )
{
    struct lusb0_enumerate_params params;
    struct usb_bus *bus;
    struct usb_device *dev, *next;
    int changes = 0;
    UINT32 i;

    if (!unix_ok) return -LUSB0_ENODEV;

    params.max = 128;
    if (!(params.devices = malloc( params.max * sizeof(*params.devices) ))) return -LUSB0_ENOMEM;
    if (WINE_UNIX_CALL( unix_lusb0_enumerate, &params ))
    {
        free( params.devices );
        return -LUSB0_EIO;
    }

    for (i = 0; i < params.count; ++i)
        get_bus( params.devices[i].bus, busses_only ? &changes : NULL );

    if (!busses_only)
    {
        /* remove devices that are gone */
        for (bus = busses; bus; bus = bus->next)
        {
            for (dev = bus->devices; dev; dev = next)
            {
                BOOL found = FALSE;

                next = dev->next;
                for (i = 0; i < params.count; ++i)
                    if (params.devices[i].bus == bus->location && params.devices[i].address == dev->devnum)
                        found = TRUE;
                if (found) continue;
                /* Keep the memory: the application may still reference it. */
                unlink_device( dev );
                changes++;
            }
        }

        /* add new devices */
        for (i = 0; i < params.count; ++i)
        {
            struct lusb0_device_info *info = &params.devices[i];

            if (!(bus = get_bus( info->bus, NULL ))) continue;
            for (dev = bus->devices; dev; dev = dev->next)
                if (dev->devnum == info->address) break;
            if (dev) continue;

            if (!(dev = calloc( 1, sizeof(*dev) ))) continue;
            dev->bus = bus;
            dev->devnum = info->address;
            memcpy( &dev->descriptor, info->device_descriptor, sizeof(dev->descriptor) );
            snprintf( dev->filename, sizeof(dev->filename), "\\\\.\\libusb0-%04u--0x%04x-0x%04x",
                      info->bus * 1000 + info->address, dev->descriptor.idVendor, dev->descriptor.idProduct );
            if (info->config_length)
                dev->config = parse_config( info->config, info->config_length );

            dev->next = bus->devices;
            if (bus->devices) bus->devices->prev = dev;
            bus->devices = dev;
            changes++;

            TRACE( "added %s config %p\n", dev->filename, dev->config );
        }
    }

    free( params.devices );
    return changes;
}

/* ---- exported API ---------------------------------------------------------- */

void CDECL usb_init(void)
{
    TRACE( "\n" );
}

void CDECL usb_set_debug( int level )
{
    TRACE( "%d\n", level );
    debug_level = level;
}

char * CDECL usb_strerror(void)
{
    return last_error;
}

int CDECL usb_find_busses(void)
{
    int ret;

    EnterCriticalSection( &lusb0_cs );
    ret = refresh_devices( TRUE );
    LeaveCriticalSection( &lusb0_cs );
    TRACE( "returning %d\n", ret );
    return set_error( ret, "usb_find_busses" );
}

int CDECL usb_find_devices(void)
{
    int ret;

    EnterCriticalSection( &lusb0_cs );
    ret = refresh_devices( FALSE );
    LeaveCriticalSection( &lusb0_cs );
    TRACE( "returning %d\n", ret );
    return set_error( ret, "usb_find_devices" );
}

struct usb_bus * CDECL usb_get_busses(void)
{
    TRACE( "returning %p\n", busses );
    return busses;
}

struct usb_device * CDECL usb_device( usb_dev_handle *dev )
{
    return dev ? dev->device : NULL;
}

usb_dev_handle * CDECL usb_open( struct usb_device *device )
{
    struct lusb0_open_params params;
    usb_dev_handle *dev;

    TRACE( "%p\n", device );
    if (!device || !device->bus || !unix_ok) return NULL;

    params.bus = device->bus->location;
    params.address = device->devnum;
    WINE_UNIX_CALL( unix_lusb0_open, &params );
    if (params.ret < 0)
    {
        set_error( params.ret, "usb_open" );
        return NULL;
    }

    if (!(dev = calloc( 1, sizeof(*dev) )))
    {
        struct lusb0_handle_params close_params = { .handle = params.handle };
        WINE_UNIX_CALL( unix_lusb0_close, &close_params );
        return NULL;
    }
    dev->unix_handle = params.handle;
    dev->device = device;
    dev->interface = -1;
    TRACE( "opened %s -> %p\n", device->filename, dev );
    return dev;
}

int CDECL usb_close( usb_dev_handle *dev )
{
    struct lusb0_handle_params params;

    TRACE( "%p\n", dev );
    if (!dev) return -LUSB0_EINVAL;
    params.handle = dev->unix_handle;
    WINE_UNIX_CALL( unix_lusb0_close, &params );
    free( dev );
    return 0;
}

static int handle_call( usb_dev_handle *dev, unsigned int code, int value, const char *func )
{
    struct lusb0_handle_params params;

    if (!dev) return set_error( -LUSB0_EINVAL, func );
    params.handle = dev->unix_handle;
    params.value = value;
    WINE_UNIX_CALL( code, &params );
    TRACE( "%s(%p, %d) -> %d\n", func, dev, value, params.ret );
    return set_error( params.ret, func );
}

int CDECL usb_set_configuration( usb_dev_handle *dev, int configuration )
{
    return handle_call( dev, unix_lusb0_set_configuration, configuration, "usb_set_configuration" );
}

int CDECL usb_claim_interface( usb_dev_handle *dev, int interface )
{
    int ret = handle_call( dev, unix_lusb0_claim_interface, interface, "usb_claim_interface" );

    if (!ret) dev->interface = interface;
    return ret;
}

int CDECL usb_release_interface( usb_dev_handle *dev, int interface )
{
    int ret = handle_call( dev, unix_lusb0_release_interface, interface, "usb_release_interface" );

    if (!ret) dev->interface = -1;
    return ret;
}

int CDECL usb_set_altinterface( usb_dev_handle *dev, int alternate )
{
    if (!dev || dev->interface < 0) return set_error( -LUSB0_EINVAL, "usb_set_altinterface" );
    return handle_call( dev, unix_lusb0_set_altinterface, (dev->interface << 8) | (alternate & 0xff),
                        "usb_set_altinterface" );
}

int CDECL usb_clear_halt( usb_dev_handle *dev, unsigned int ep )
{
    return handle_call( dev, unix_lusb0_clear_halt, ep, "usb_clear_halt" );
}

int CDECL usb_resetep( usb_dev_handle *dev, unsigned int ep )
{
    return usb_clear_halt( dev, ep );
}

int CDECL usb_reset( usb_dev_handle *dev )
{
    return handle_call( dev, unix_lusb0_reset, 0, "usb_reset" );
}

int CDECL usb_get_string( usb_dev_handle *dev, int index, int langid, char *buf, size_t buflen )
{
    struct lusb0_string_params params;

    if (!dev || !buf) return set_error( -LUSB0_EINVAL, "usb_get_string" );
    params.handle = dev->unix_handle;
    params.index = index;
    params.langid = langid;
    params.buffer = buf;
    params.length = buflen;
    WINE_UNIX_CALL( unix_lusb0_get_string, &params );
    return set_error( params.ret, "usb_get_string" );
}

int CDECL usb_get_string_simple( usb_dev_handle *dev, int index, char *buf, size_t buflen )
{
    struct lusb0_string_params params;

    if (!dev || !buf || !buflen) return set_error( -LUSB0_EINVAL, "usb_get_string_simple" );
    params.handle = dev->unix_handle;
    params.index = index;
    params.langid = 0;
    params.buffer = buf;
    params.length = buflen;
    WINE_UNIX_CALL( unix_lusb0_get_string, &params );
    if (params.ret >= 0) buf[min( (size_t)params.ret, buflen - 1 )] = 0;
    TRACE( "%p %d -> %d %s\n", dev, index, params.ret, params.ret >= 0 ? debugstr_a( buf ) : "" );
    return set_error( params.ret, "usb_get_string_simple" );
}

static int transfer( usb_dev_handle *dev, int ep, char *bytes, int size, int timeout, const char *func )
{
    struct lusb0_transfer_params params;

    if (!dev || (size && !bytes) || size < 0) return set_error( -LUSB0_EINVAL, func );
    params.handle = dev->unix_handle;
    params.endpoint = ep;
    params.buffer = bytes;
    params.length = size;
    params.timeout = timeout;
    WINE_UNIX_CALL( unix_lusb0_bulk_transfer, &params );
    TRACE( "%s(%p, %#x, %d, %d) -> %d\n", func, dev, ep, size, timeout, params.ret );
    return set_error( params.ret, func );
}

int CDECL usb_bulk_write( usb_dev_handle *dev, int ep, char *bytes, int size, int timeout )
{
    return transfer( dev, ep & ~0x80, bytes, size, timeout, "usb_bulk_write" );
}

int CDECL usb_bulk_read( usb_dev_handle *dev, int ep, char *bytes, int size, int timeout )
{
    return transfer( dev, ep | 0x80, bytes, size, timeout, "usb_bulk_read" );
}

int CDECL usb_interrupt_write( usb_dev_handle *dev, int ep, char *bytes, int size, int timeout )
{
    return transfer( dev, ep & ~0x80, bytes, size, timeout, "usb_interrupt_write" );
}

int CDECL usb_interrupt_read( usb_dev_handle *dev, int ep, char *bytes, int size, int timeout )
{
    return transfer( dev, ep | 0x80, bytes, size, timeout, "usb_interrupt_read" );
}

int CDECL usb_control_msg( usb_dev_handle *dev, int requesttype, int request, int value, int index,
                           char *bytes, int size, int timeout )
{
    struct lusb0_control_params params;

    if (!dev || (size && !bytes) || size < 0) return set_error( -LUSB0_EINVAL, "usb_control_msg" );
    params.handle = dev->unix_handle;
    params.request_type = requesttype;
    params.request = request;
    params.value = value;
    params.index = index;
    params.buffer = bytes;
    params.length = size;
    params.timeout = timeout;
    WINE_UNIX_CALL( unix_lusb0_control_transfer, &params );
    TRACE( "%p %#x %#x -> %d\n", dev, requesttype, request, params.ret );
    return set_error( params.ret, "usb_control_msg" );
}

static int setup_async( usb_dev_handle *dev, void **context, unsigned char ep, UINT8 type, const char *func )
{
    struct lusb0_async_submit_params params;
    struct async_context *async;

    if (!dev || !context) return set_error( -LUSB0_EINVAL, func );
    *context = NULL;

    params.handle = dev->unix_handle;
    params.context = 0;
    params.endpoint = ep;
    params.type = type;
    params.buffer = NULL;
    params.length = 0;
    WINE_UNIX_CALL( unix_lusb0_async_submit, &params );
    if (params.ret < 0) return set_error( params.ret, func );

    if (!(async = calloc( 1, sizeof(*async) )))
    {
        struct lusb0_async_params free_params = { .context = params.context };
        WINE_UNIX_CALL( unix_lusb0_async_free, &free_params );
        return set_error( -LUSB0_ENOMEM, func );
    }
    async->dev = dev;
    async->unix_context = params.context;
    *context = async;
    TRACE( "%s(%p, %#x) -> %p\n", func, dev, ep, async );
    return 0;
}

int CDECL usb_bulk_setup_async( usb_dev_handle *dev, void **context, unsigned char ep )
{
    return setup_async( dev, context, ep, LUSB0_ASYNC_BULK, "usb_bulk_setup_async" );
}

int CDECL usb_interrupt_setup_async( usb_dev_handle *dev, void **context, unsigned char ep )
{
    return setup_async( dev, context, ep, LUSB0_ASYNC_INTERRUPT, "usb_interrupt_setup_async" );
}

int CDECL usb_submit_async( void *context, char *bytes, int size )
{
    struct async_context *async = context;
    struct lusb0_async_submit_params params;

    if (!async || (size && !bytes) || size < 0) return set_error( -LUSB0_EINVAL, "usb_submit_async" );
    params.handle = async->dev->unix_handle;
    params.context = async->unix_context;
    params.endpoint = 0;
    params.type = 0;
    params.buffer = bytes;
    params.length = size;
    WINE_UNIX_CALL( unix_lusb0_async_submit, &params );
    TRACE( "%p %d -> %d\n", async, size, params.ret );
    return set_error( params.ret, "usb_submit_async" );
}

static int reap_async( void *context, int timeout, int cancel, const char *func )
{
    struct async_context *async = context;
    struct lusb0_async_params params;

    if (!async) return set_error( -LUSB0_EINVAL, func );
    params.context = async->unix_context;
    params.timeout = timeout;
    params.cancel = cancel;
    WINE_UNIX_CALL( unix_lusb0_async_reap, &params );
    if (params.ret != -LUSB0_ETIMEDOUT) TRACE( "%s(%p, %d) -> %d\n", func, async, timeout, params.ret );
    /* timeouts are routine while polling, do not record them as errors */
    return params.ret == -LUSB0_ETIMEDOUT ? params.ret : set_error( params.ret, func );
}

int CDECL usb_reap_async( void *context, int timeout )
{
    return reap_async( context, timeout, 1, "usb_reap_async" );
}

int CDECL usb_reap_async_nocancel( void *context, int timeout )
{
    return reap_async( context, timeout, 0, "usb_reap_async_nocancel" );
}

int CDECL usb_cancel_async( void *context )
{
    struct async_context *async = context;
    struct lusb0_async_params params;

    if (!async) return set_error( -LUSB0_EINVAL, "usb_cancel_async" );
    params.context = async->unix_context;
    WINE_UNIX_CALL( unix_lusb0_async_cancel, &params );
    TRACE( "%p -> %d\n", async, params.ret );
    return params.ret;
}

int CDECL usb_free_async( void **context )
{
    struct async_context *async;
    struct lusb0_async_params params;

    if (!context || !(async = *context)) return set_error( -LUSB0_EINVAL, "usb_free_async" );
    params.context = async->unix_context;
    WINE_UNIX_CALL( unix_lusb0_async_free, &params );
    free( async );
    *context = NULL;
    TRACE( "%p freed\n", async );
    return 0;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls( instance );
        if (__wine_init_unix_call())
        {
            WARN( "no Unix library available, USB devices will not be found\n" );
            break;
        }
        unix_ok = !WINE_UNIX_CALL( unix_lusb0_init, NULL );
        TRACE( "unix library %s\n", unix_ok ? "initialized" : "failed" );
        break;
    }
    return TRUE;
}
