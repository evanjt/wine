/* windows.Devices.Bluetooth.Advertisement Implementation
 *
 * Copyright 2025 Vibhav Pant
 * Copyright 2026 Evan Thomas
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

#define WIDL_using_Windows_Storage_Streams
#include "private.h"
#include "windows.storage.streams.h"
#include "initguid.h"
#include "robuffer.h"
#include "roapi.h"
#include "wine/winebth.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

/* --- Helpers --- */

static HRESULT buffer_create( const BYTE *data, UINT32 size, IBuffer **out )
{
    static const WCHAR class_name[] = L"Windows.Storage.Streams.Buffer";
    IBufferByteAccess *access;
    IBufferFactory *factory;
    HSTRING_HEADER hdr;
    HSTRING str;
    HRESULT hr;
    BYTE *bytes;

    *out = NULL;
    if (FAILED((hr = WindowsCreateStringReference( class_name, ARRAY_SIZE( class_name ) - 1, &hdr, &str )))) return hr;
    if (FAILED((hr = RoGetActivationFactory( str, &IID_IBufferFactory, (void **)&factory )))) return hr;
    hr = IBufferFactory_Create( factory, size, out );
    IBufferFactory_Release( factory );
    if (FAILED(hr)) return hr;

    if (FAILED((hr = IBuffer_QueryInterface( *out, &IID_IBufferByteAccess, (void **)&access ))))
    {
        IBuffer_Release( *out );
        *out = NULL;
        return hr;
    }
    hr = IBufferByteAccess_Buffer( access, &bytes );
    IBufferByteAccess_Release( access );
    if (SUCCEEDED(hr))
    {
        memcpy( bytes, data, size );
        hr = IBuffer_put_Length( *out, size );
    }
    if (FAILED(hr))
    {
        IBuffer_Release( *out );
        *out = NULL;
    }
    return hr;
}

static HRESULT class_name_string( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

/* A UUID in the Bluetooth base range that fits in 16 or 32 bits. */
static BOOL uuid_short_value( const GUID *uuid, UINT32 *value )
{
    static const GUID base = { 0, 0, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
    if (uuid->Data2 != base.Data2 || uuid->Data3 != base.Data3 || memcmp( uuid->Data4, base.Data4, sizeof( base.Data4 ) ))
        return FALSE;
    *value = uuid->Data1;
    return TRUE;
}

/* Bluetooth transmits 128-bit UUIDs least significant byte first. */
static void uuid_to_le_bytes( const GUID *uuid, BYTE out[16] )
{
    int i;
    for (i = 0; i < 8; i++) out[i] = uuid->Data4[7 - i];
    out[8] = uuid->Data3 & 0xff;
    out[9] = uuid->Data3 >> 8;
    out[10] = uuid->Data2 & 0xff;
    out[11] = uuid->Data2 >> 8;
    out[12] = uuid->Data1 & 0xff;
    out[13] = (uuid->Data1 >> 8) & 0xff;
    out[14] = (uuid->Data1 >> 16) & 0xff;
    out[15] = uuid->Data1 >> 24;
}

/* --- IVector<GUID> --- */

struct guid_vector
{
    IVector_GUID IVector_GUID_iface;
    IVectorView_GUID IVectorView_GUID_iface;
    IIterable_GUID IIterable_GUID_iface;
    LONG ref;
    BOOL view;
    GUID *items;
    UINT32 count;
    UINT32 capacity;
};

static inline struct guid_vector *impl_from_IVector_GUID( IVector_GUID *iface )
{
    return CONTAINING_RECORD( iface, struct guid_vector, IVector_GUID_iface );
}

static HRESULT WINAPI guid_vector_QueryInterface( IVector_GUID *iface, REFIID iid, void **out )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        (!impl->view && IsEqualGUID( iid, &IID_IVector_GUID )))
    {
        IVector_GUID_AddRef(( *out = &impl->IVector_GUID_iface ));
        return S_OK;
    }
    if (impl->view && IsEqualGUID( iid, &IID_IVectorView_GUID ))
    {
        IVector_GUID_AddRef( iface );
        *out = &impl->IVectorView_GUID_iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_GUID ))
    {
        IVector_GUID_AddRef( iface );
        *out = &impl->IIterable_GUID_iface;
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI guid_vector_AddRef( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI guid_vector_Release( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        free( impl->items );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI guid_vector_GetIids( IVector_GUID *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI guid_vector_GetRuntimeClassName( IVector_GUID *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Foundation.Collections.IVector`1<Guid>", class_name );
}

static HRESULT WINAPI guid_vector_GetTrustLevel( IVector_GUID *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetAt( IVector_GUID *iface, UINT32 index, GUID *value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    *value = impl->items[index];
    return S_OK;
}

static HRESULT WINAPI guid_vector_get_Size( IVector_GUID *iface, UINT32 *value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetView( IVector_GUID *iface, IVectorView_GUID **value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface ), *view;
    HRESULT hr;

    TRACE( "(%p, %p)\n", iface, value );

    if (FAILED((hr = guid_vector_create( impl->items, impl->count, TRUE, &view )))) return hr;
    *value = &view->IVectorView_GUID_iface;
    return S_OK;
}

static HRESULT WINAPI guid_vector_IndexOf( IVector_GUID *iface, GUID element, UINT32 *index, BOOLEAN *found )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    UINT32 i;

    *found = FALSE;
    *index = 0;
    for (i = 0; i < impl->count; i++)
    {
        if (IsEqualGUID( &impl->items[i], &element ))
        {
            *index = i;
            *found = TRUE;
            break;
        }
    }
    return S_OK;
}

static HRESULT guid_vector_reserve( struct guid_vector *impl, UINT32 count )
{
    GUID *items;
    UINT32 capacity;

    if (count <= impl->capacity) return S_OK;
    capacity = max( count, impl->capacity * 2 );
    if (!(items = realloc( impl->items, capacity * sizeof( *items ) ))) return E_OUTOFMEMORY;
    impl->items = items;
    impl->capacity = capacity;
    return S_OK;
}

static HRESULT WINAPI guid_vector_SetAt( IVector_GUID *iface, UINT32 index, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    impl->items[index] = value;
    return S_OK;
}

static HRESULT WINAPI guid_vector_InsertAt( IVector_GUID *iface, UINT32 index, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    HRESULT hr;

    if (index > impl->count) return E_BOUNDS;
    if (FAILED((hr = guid_vector_reserve( impl, impl->count + 1 )))) return hr;
    memmove( &impl->items[index + 1], &impl->items[index], (impl->count - index) * sizeof( GUID ) );
    impl->items[index] = value;
    impl->count++;
    return S_OK;
}

static HRESULT WINAPI guid_vector_RemoveAt( IVector_GUID *iface, UINT32 index )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (index >= impl->count) return E_BOUNDS;
    memmove( &impl->items[index], &impl->items[index + 1], (impl->count - index - 1) * sizeof( GUID ) );
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI guid_vector_Append( IVector_GUID *iface, GUID value )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    return guid_vector_InsertAt( iface, impl->count, value );
}

static HRESULT WINAPI guid_vector_RemoveAtEnd( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    if (!impl->count) return E_BOUNDS;
    impl->count--;
    return S_OK;
}

static HRESULT WINAPI guid_vector_Clear( IVector_GUID *iface )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    impl->count = 0;
    return S_OK;
}

static HRESULT WINAPI guid_vector_GetMany( IVector_GUID *iface, UINT32 start_index, UINT32 items_size, GUID *items,
                                           UINT32 *count )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );

    if (start_index > impl->count) return E_BOUNDS;
    *count = min( items_size, impl->count - start_index );
    memcpy( items, &impl->items[start_index], *count * sizeof( GUID ) );
    return S_OK;
}

static HRESULT WINAPI guid_vector_ReplaceAll( IVector_GUID *iface, UINT32 count, GUID *items )
{
    struct guid_vector *impl = impl_from_IVector_GUID( iface );
    HRESULT hr;

    if (FAILED((hr = guid_vector_reserve( impl, count )))) return hr;
    memcpy( impl->items, items, count * sizeof( GUID ) );
    impl->count = count;
    return S_OK;
}

static const IVector_GUIDVtbl guid_vector_vtbl =
{
    guid_vector_QueryInterface,
    guid_vector_AddRef,
    guid_vector_Release,
    guid_vector_GetIids,
    guid_vector_GetRuntimeClassName,
    guid_vector_GetTrustLevel,
    guid_vector_GetAt,
    guid_vector_get_Size,
    guid_vector_GetView,
    guid_vector_IndexOf,
    guid_vector_SetAt,
    guid_vector_InsertAt,
    guid_vector_RemoveAt,
    guid_vector_Append,
    guid_vector_RemoveAtEnd,
    guid_vector_Clear,
    guid_vector_GetMany,
    guid_vector_ReplaceAll,
};

DEFINE_IINSPECTABLE( guid_vector_view, IVectorView_GUID, struct guid_vector, IVector_GUID_iface )

static HRESULT WINAPI guid_vector_view_GetAt( IVectorView_GUID *iface, UINT32 index, GUID *value )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_GetAt( &impl->IVector_GUID_iface, index, value );
}

static HRESULT WINAPI guid_vector_view_get_Size( IVectorView_GUID *iface, UINT32 *value )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_get_Size( &impl->IVector_GUID_iface, value );
}

static HRESULT WINAPI guid_vector_view_IndexOf( IVectorView_GUID *iface, GUID element, UINT32 *index, BOOLEAN *found )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_IndexOf( &impl->IVector_GUID_iface, element, index, found );
}

static HRESULT WINAPI guid_vector_view_GetMany( IVectorView_GUID *iface, UINT32 start_index, UINT32 items_size,
                                                GUID *items, UINT32 *count )
{
    struct guid_vector *impl = impl_from_IVectorView_GUID( iface );
    return guid_vector_GetMany( &impl->IVector_GUID_iface, start_index, items_size, items, count );
}

static const IVectorView_GUIDVtbl guid_vector_view_vtbl =
{
    guid_vector_view_QueryInterface,
    guid_vector_view_AddRef,
    guid_vector_view_Release,
    guid_vector_view_GetIids,
    guid_vector_view_GetRuntimeClassName,
    guid_vector_view_GetTrustLevel,
    guid_vector_view_GetAt,
    guid_vector_view_get_Size,
    guid_vector_view_IndexOf,
    guid_vector_view_GetMany,
};

struct guid_iterator
{
    IIterator_GUID IIterator_GUID_iface;
    LONG ref;
    struct guid_vector *vector;
    UINT32 index;
};

static inline struct guid_iterator *impl_from_IIterator_GUID( IIterator_GUID *iface )
{
    return CONTAINING_RECORD( iface, struct guid_iterator, IIterator_GUID_iface );
}

static HRESULT WINAPI guid_iterator_QueryInterface( IIterator_GUID *iface, REFIID iid, void **out )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_GUID ))
    {
        IIterator_GUID_AddRef(( *out = &impl->IIterator_GUID_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI guid_iterator_AddRef( IIterator_GUID *iface )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI guid_iterator_Release( IIterator_GUID *iface )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IVector_GUID_Release( &impl->vector->IVector_GUID_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI guid_iterator_GetIids( IIterator_GUID *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI guid_iterator_GetRuntimeClassName( IIterator_GUID *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Foundation.Collections.IIterator`1<Guid>", class_name );
}

static HRESULT WINAPI guid_iterator_GetTrustLevel( IIterator_GUID *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_get_Current( IIterator_GUID *iface, GUID *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    return guid_vector_GetAt( &impl->vector->IVector_GUID_iface, impl->index, value );
}

static HRESULT WINAPI guid_iterator_get_HasCurrent( IIterator_GUID *iface, boolean *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_MoveNext( IIterator_GUID *iface, boolean *value )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    if (impl->index < impl->vector->count) impl->index++;
    *value = impl->index < impl->vector->count;
    return S_OK;
}

static HRESULT WINAPI guid_iterator_GetMany( IIterator_GUID *iface, UINT32 items_size, GUID *items, UINT32 *count )
{
    struct guid_iterator *impl = impl_from_IIterator_GUID( iface );
    HRESULT hr;

    hr = guid_vector_GetMany( &impl->vector->IVector_GUID_iface, impl->index, items_size, items, count );
    if (SUCCEEDED(hr)) impl->index += *count;
    return hr;
}

static const IIterator_GUIDVtbl guid_iterator_vtbl =
{
    guid_iterator_QueryInterface,
    guid_iterator_AddRef,
    guid_iterator_Release,
    guid_iterator_GetIids,
    guid_iterator_GetRuntimeClassName,
    guid_iterator_GetTrustLevel,
    guid_iterator_get_Current,
    guid_iterator_get_HasCurrent,
    guid_iterator_MoveNext,
    guid_iterator_GetMany,
};

DEFINE_IINSPECTABLE( guid_iterable, IIterable_GUID, struct guid_vector, IVector_GUID_iface )

static HRESULT WINAPI guid_iterable_First( IIterable_GUID *iface, IIterator_GUID **value )
{
    struct guid_vector *impl = impl_from_IIterable_GUID( iface );
    struct guid_iterator *iter;

    TRACE( "(%p, %p)\n", iface, value );

    if (!(iter = calloc( 1, sizeof( *iter ) ))) return E_OUTOFMEMORY;
    iter->IIterator_GUID_iface.lpVtbl = &guid_iterator_vtbl;
    iter->ref = 1;
    iter->vector = impl;
    IVector_GUID_AddRef( &impl->IVector_GUID_iface );
    *value = &iter->IIterator_GUID_iface;
    return S_OK;
}

static const IIterable_GUIDVtbl guid_iterable_vtbl =
{
    guid_iterable_QueryInterface,
    guid_iterable_AddRef,
    guid_iterable_Release,
    guid_iterable_GetIids,
    guid_iterable_GetRuntimeClassName,
    guid_iterable_GetTrustLevel,
    guid_iterable_First,
};

HRESULT guid_vector_create( const GUID *items, UINT32 count, BOOL view, struct guid_vector **out )
{
    struct guid_vector *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IVector_GUID_iface.lpVtbl = &guid_vector_vtbl;
    impl->IVectorView_GUID_iface.lpVtbl = &guid_vector_view_vtbl;
    impl->IIterable_GUID_iface.lpVtbl = &guid_iterable_vtbl;
    impl->ref = 1;
    impl->view = view;
    if (count)
    {
        if (!(impl->items = malloc( count * sizeof( GUID ) )))
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
        memcpy( impl->items, items, count * sizeof( GUID ) );
        impl->count = impl->capacity = count;
    }
    *out = impl;
    return S_OK;
}

/* --- BluetoothLEManufacturerData --- */

struct manufacturer_data
{
    IBluetoothLEManufacturerData IBluetoothLEManufacturerData_iface;
    LONG ref;
    UINT16 company_id;
    IBuffer *data;
};

static inline struct manufacturer_data *impl_from_IBluetoothLEManufacturerData( IBluetoothLEManufacturerData *iface )
{
    return CONTAINING_RECORD( iface, struct manufacturer_data, IBluetoothLEManufacturerData_iface );
}

static HRESULT WINAPI manufacturer_data_QueryInterface( IBluetoothLEManufacturerData *iface, REFIID iid, void **out )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEManufacturerData ))
    {
        IBluetoothLEManufacturerData_AddRef(( *out = &impl->IBluetoothLEManufacturerData_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI manufacturer_data_AddRef( IBluetoothLEManufacturerData *iface )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI manufacturer_data_Release( IBluetoothLEManufacturerData *iface )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->data) IBuffer_Release( impl->data );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI manufacturer_data_GetIids( IBluetoothLEManufacturerData *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI manufacturer_data_GetRuntimeClassName( IBluetoothLEManufacturerData *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEManufacturerData", class_name );
}

static HRESULT WINAPI manufacturer_data_GetTrustLevel( IBluetoothLEManufacturerData *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_get_CompanyId( IBluetoothLEManufacturerData *iface, UINT16 *value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->company_id;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_put_CompanyId( IBluetoothLEManufacturerData *iface, UINT16 value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %u)\n", iface, value );
    impl->company_id = value;
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_get_Data( IBluetoothLEManufacturerData *iface, IBuffer **value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->data)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI manufacturer_data_put_Data( IBluetoothLEManufacturerData *iface, IBuffer *value )
{
    struct manufacturer_data *impl = impl_from_IBluetoothLEManufacturerData( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IBuffer_AddRef( value );
    if (impl->data) IBuffer_Release( impl->data );
    impl->data = value;
    return S_OK;
}

static const IBluetoothLEManufacturerDataVtbl manufacturer_data_vtbl =
{
    manufacturer_data_QueryInterface,
    manufacturer_data_AddRef,
    manufacturer_data_Release,
    manufacturer_data_GetIids,
    manufacturer_data_GetRuntimeClassName,
    manufacturer_data_GetTrustLevel,
    manufacturer_data_get_CompanyId,
    manufacturer_data_put_CompanyId,
    manufacturer_data_get_Data,
    manufacturer_data_put_Data,
};

HRESULT manufacturer_data_create( UINT16 company_id, const BYTE *data, UINT32 size,
                                  IBluetoothLEManufacturerData **out )
{
    struct manufacturer_data *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEManufacturerData_iface.lpVtbl = &manufacturer_data_vtbl;
    impl->ref = 1;
    impl->company_id = company_id;
    if (FAILED((hr = buffer_create( data, size, &impl->data ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothLEManufacturerData_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisementDataSection --- */

struct data_section
{
    IBluetoothLEAdvertisementDataSection IBluetoothLEAdvertisementDataSection_iface;
    LONG ref;
    BYTE type;
    IBuffer *data;
};

static inline struct data_section *impl_from_IBluetoothLEAdvertisementDataSection( IBluetoothLEAdvertisementDataSection *iface )
{
    return CONTAINING_RECORD( iface, struct data_section, IBluetoothLEAdvertisementDataSection_iface );
}

static HRESULT WINAPI data_section_QueryInterface( IBluetoothLEAdvertisementDataSection *iface, REFIID iid, void **out )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementDataSection ))
    {
        IBluetoothLEAdvertisementDataSection_AddRef(( *out = &impl->IBluetoothLEAdvertisementDataSection_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI data_section_AddRef( IBluetoothLEAdvertisementDataSection *iface )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI data_section_Release( IBluetoothLEAdvertisementDataSection *iface )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->data) IBuffer_Release( impl->data );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_section_GetIids( IBluetoothLEAdvertisementDataSection *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_section_GetRuntimeClassName( IBluetoothLEAdvertisementDataSection *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementDataSection", class_name );
}

static HRESULT WINAPI data_section_GetTrustLevel( IBluetoothLEAdvertisementDataSection *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI data_section_get_DataType( IBluetoothLEAdvertisementDataSection *iface, BYTE *value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->type;
    return S_OK;
}

static HRESULT WINAPI data_section_put_DataType( IBluetoothLEAdvertisementDataSection *iface, BYTE value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %u)\n", iface, value );
    impl->type = value;
    return S_OK;
}

static HRESULT WINAPI data_section_get_Data( IBluetoothLEAdvertisementDataSection *iface, IBuffer **value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->data)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI data_section_put_Data( IBluetoothLEAdvertisementDataSection *iface, IBuffer *value )
{
    struct data_section *impl = impl_from_IBluetoothLEAdvertisementDataSection( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IBuffer_AddRef( value );
    if (impl->data) IBuffer_Release( impl->data );
    impl->data = value;
    return S_OK;
}

static const IBluetoothLEAdvertisementDataSectionVtbl data_section_vtbl =
{
    data_section_QueryInterface,
    data_section_AddRef,
    data_section_Release,
    data_section_GetIids,
    data_section_GetRuntimeClassName,
    data_section_GetTrustLevel,
    data_section_get_DataType,
    data_section_put_DataType,
    data_section_get_Data,
    data_section_put_Data,
};

HRESULT data_section_create( BYTE type, const BYTE *data, UINT32 size, IBluetoothLEAdvertisementDataSection **out )
{
    struct data_section *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementDataSection_iface.lpVtbl = &data_section_vtbl;
    impl->ref = 1;
    impl->type = type;
    if (FAILED((hr = buffer_create( data, size, &impl->data ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothLEAdvertisementDataSection_iface;
    return S_OK;
}

/* --- BluetoothLEAdvertisement --- */

static const struct vector_iids manufacturer_data_vector_iids =
{
    .iterable = &IID_IIterable_BluetoothLEManufacturerData,
    .iterator = &IID_IIterator_BluetoothLEManufacturerData,
    .vector = &IID_IVector_BluetoothLEManufacturerData,
    .view = &IID_IVectorView_BluetoothLEManufacturerData,
};

static const struct vector_iids data_section_vector_iids =
{
    .iterable = &IID_IIterable_BluetoothLEAdvertisementDataSection,
    .iterator = &IID_IIterator_BluetoothLEAdvertisementDataSection,
    .vector = &IID_IVector_BluetoothLEAdvertisementDataSection,
    .view = &IID_IVectorView_BluetoothLEAdvertisementDataSection,
};

struct advertisement
{
    IBluetoothLEAdvertisement IBluetoothLEAdvertisement_iface;
    LONG ref;
    IReference_BluetoothLEAdvertisementFlags *flags;
    HSTRING local_name;
    struct guid_vector *uuids;
    IVector_IInspectable *manufacturer_data;
    IVector_IInspectable *sections;
};

static inline struct advertisement *impl_from_IBluetoothLEAdvertisement( IBluetoothLEAdvertisement *iface )
{
    return CONTAINING_RECORD( iface, struct advertisement, IBluetoothLEAdvertisement_iface );
}

static HRESULT WINAPI advertisement_QueryInterface( IBluetoothLEAdvertisement *iface, REFIID iid, void **out )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisement ))
    {
        IBluetoothLEAdvertisement_AddRef(( *out = &impl->IBluetoothLEAdvertisement_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI advertisement_AddRef( IBluetoothLEAdvertisement *iface )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI advertisement_Release( IBluetoothLEAdvertisement *iface )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->flags) IReference_BluetoothLEAdvertisementFlags_Release( impl->flags );
        WindowsDeleteString( impl->local_name );
        if (impl->uuids) IVector_GUID_Release( &impl->uuids->IVector_GUID_iface );
        if (impl->manufacturer_data) IVector_IInspectable_Release( impl->manufacturer_data );
        if (impl->sections) IVector_IInspectable_Release( impl->sections );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI advertisement_GetIids( IBluetoothLEAdvertisement *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI advertisement_GetRuntimeClassName( IBluetoothLEAdvertisement *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisement", class_name );
}

static HRESULT WINAPI advertisement_GetTrustLevel( IBluetoothLEAdvertisement *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_Flags( IBluetoothLEAdvertisement *iface, IReference_BluetoothLEAdvertisementFlags **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->flags)) IReference_BluetoothLEAdvertisementFlags_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI advertisement_put_Flags( IBluetoothLEAdvertisement *iface, IReference_BluetoothLEAdvertisementFlags *value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (value) IReference_BluetoothLEAdvertisementFlags_AddRef( value );
    if (impl->flags) IReference_BluetoothLEAdvertisementFlags_Release( impl->flags );
    impl->flags = value;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_LocalName( IBluetoothLEAdvertisement *iface, HSTRING *value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->local_name, value );
}

static HRESULT WINAPI advertisement_put_LocalName( IBluetoothLEAdvertisement *iface, HSTRING value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    HSTRING copy;
    HRESULT hr;

    TRACE( "(%p, %s)\n", iface, debugstr_hstring( value ) );

    if (FAILED((hr = WindowsDuplicateString( value, &copy )))) return hr;
    WindowsDeleteString( impl->local_name );
    impl->local_name = copy;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_ServiceUuids( IBluetoothLEAdvertisement *iface, IVector_GUID **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_GUID_AddRef(( *value = &impl->uuids->IVector_GUID_iface ));
    return S_OK;
}

static HRESULT WINAPI advertisement_get_ManufacturerData( IBluetoothLEAdvertisement *iface,
                                                          IVector_BluetoothLEManufacturerData **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_IInspectable_AddRef( impl->manufacturer_data );
    *value = (IVector_BluetoothLEManufacturerData *)impl->manufacturer_data;
    return S_OK;
}

static HRESULT WINAPI advertisement_get_DataSections( IBluetoothLEAdvertisement *iface,
                                                      IVector_BluetoothLEAdvertisementDataSection **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IVector_IInspectable_AddRef( impl->sections );
    *value = (IVector_BluetoothLEAdvertisementDataSection *)impl->sections;
    return S_OK;
}

static HRESULT WINAPI advertisement_GetManufacturerDataByCompanyId( IBluetoothLEAdvertisement *iface, UINT16 id,
                                                                    IVectorView_BluetoothLEManufacturerData **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    IVector_IInspectable *matches;
    UINT32 i, size;
    HRESULT hr;

    TRACE( "(%p, %u, %p)\n", iface, id, value );

    if (FAILED((hr = vector_create( &manufacturer_data_vector_iids, (void **)&matches )))) return hr;
    IVector_IInspectable_get_Size( impl->manufacturer_data, &size );
    for (i = 0; i < size && SUCCEEDED(hr); i++)
    {
        IBluetoothLEManufacturerData *data;
        UINT16 company_id;

        if (FAILED(IVector_IInspectable_GetAt( impl->manufacturer_data, i, (IInspectable **)&data ))) continue;
        if (SUCCEEDED(IBluetoothLEManufacturerData_get_CompanyId( data, &company_id )) && company_id == id)
            hr = IVector_IInspectable_Append( matches, (IInspectable *)data );
        IBluetoothLEManufacturerData_Release( data );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( matches, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( matches );
    return hr;
}

static HRESULT WINAPI advertisement_GetSectionsByType( IBluetoothLEAdvertisement *iface, BYTE type,
                                                       IVectorView_BluetoothLEAdvertisementDataSection **value )
{
    struct advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    IVector_IInspectable *matches;
    UINT32 i, size;
    HRESULT hr;

    TRACE( "(%p, %#x, %p)\n", iface, type, value );

    if (FAILED((hr = vector_create( &data_section_vector_iids, (void **)&matches )))) return hr;
    IVector_IInspectable_get_Size( impl->sections, &size );
    for (i = 0; i < size && SUCCEEDED(hr); i++)
    {
        IBluetoothLEAdvertisementDataSection *section;
        BYTE section_type;

        if (FAILED(IVector_IInspectable_GetAt( impl->sections, i, (IInspectable **)&section ))) continue;
        if (SUCCEEDED(IBluetoothLEAdvertisementDataSection_get_DataType( section, &section_type )) && section_type == type)
            hr = IVector_IInspectable_Append( matches, (IInspectable *)section );
        IBluetoothLEAdvertisementDataSection_Release( section );
    }
    if (SUCCEEDED(hr)) hr = IVector_IInspectable_GetView( matches, (IVectorView_IInspectable **)value );
    IVector_IInspectable_Release( matches );
    return hr;
}

static const IBluetoothLEAdvertisementVtbl advertisement_vtbl =
{
    advertisement_QueryInterface,
    advertisement_AddRef,
    advertisement_Release,
    advertisement_GetIids,
    advertisement_GetRuntimeClassName,
    advertisement_GetTrustLevel,
    advertisement_get_Flags,
    advertisement_put_Flags,
    advertisement_get_LocalName,
    advertisement_put_LocalName,
    advertisement_get_ServiceUuids,
    advertisement_get_ManufacturerData,
    advertisement_get_DataSections,
    advertisement_GetManufacturerDataByCompanyId,
    advertisement_GetSectionsByType,
};

static HRESULT advertisement_add_section( struct advertisement *impl, BYTE type, const BYTE *data, UINT32 size )
{
    IBluetoothLEAdvertisementDataSection *section;
    HRESULT hr;

    if (FAILED((hr = data_section_create( type, data, size, &section )))) return hr;
    hr = IVector_IInspectable_Append( impl->sections, (IInspectable *)section );
    IBluetoothLEAdvertisementDataSection_Release( section );
    return hr;
}

/* Advertising data types from the Bluetooth Core Specification Supplement. */
#define AD_TYPE_UUID16_COMPLETE  0x03
#define AD_TYPE_UUID32_COMPLETE  0x05
#define AD_TYPE_UUID128_COMPLETE 0x07
#define AD_TYPE_LOCAL_NAME       0x09
#define AD_TYPE_TX_POWER         0x0a
#define AD_TYPE_SERVICE_DATA16   0x16
#define AD_TYPE_APPEARANCE       0x19
#define AD_TYPE_SERVICE_DATA32   0x20
#define AD_TYPE_SERVICE_DATA128  0x21
#define AD_TYPE_MANUFACTURER     0xff

static HRESULT advertisement_add_uuid_sections( struct advertisement *impl, const struct winebth_le_advertisement *adv )
{
    BYTE uuid16[WINEBTH_LE_ADV_MAX_UUIDS * 2], uuid32[WINEBTH_LE_ADV_MAX_UUIDS * 4], uuid128[WINEBTH_LE_ADV_MAX_UUIDS * 16];
    UINT32 n16 = 0, n32 = 0, n128 = 0, i, value;
    HRESULT hr = S_OK;

    for (i = 0; i < adv->uuid_count; i++)
    {
        if (uuid_short_value( &adv->uuids[i], &value ))
        {
            if (value <= 0xffff)
            {
                uuid16[n16 * 2] = value & 0xff;
                uuid16[n16 * 2 + 1] = value >> 8;
                n16++;
            }
            else
            {
                memcpy( &uuid32[n32 * 4], &value, 4 );
                n32++;
            }
        }
        else
            uuid_to_le_bytes( &adv->uuids[i], &uuid128[n128++ * 16] );
    }
    if (n16 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID16_COMPLETE, uuid16, n16 * 2 );
    if (n32 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID32_COMPLETE, uuid32, n32 * 4 );
    if (n128 && SUCCEEDED(hr)) hr = advertisement_add_section( impl, AD_TYPE_UUID128_COMPLETE, uuid128, n128 * 16 );
    return hr;
}

HRESULT advertisement_create( const struct winebth_le_advertisement *adv, IBluetoothLEAdvertisement **out )
{
    struct advertisement *impl;
    HRESULT hr;
    UINT32 i;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisement_iface.lpVtbl = &advertisement_vtbl;
    impl->ref = 1;

    if (FAILED((hr = guid_vector_create( adv->uuids, adv->uuid_count, FALSE, &impl->uuids )))) goto failed;
    if (FAILED((hr = vector_create( &manufacturer_data_vector_iids, (void **)&impl->manufacturer_data )))) goto failed;
    if (FAILED((hr = vector_create( &data_section_vector_iids, (void **)&impl->sections )))) goto failed;

    if (adv->flags & WINEBTH_LE_ADV_FLAG_NAME && adv->name[0])
    {
        WCHAR name[BLUETOOTH_MAX_NAME_SIZE];
        int len = MultiByteToWideChar( CP_UTF8, 0, adv->name, -1, name, ARRAY_SIZE( name ) );
        if (len > 0 && FAILED((hr = WindowsCreateString( name, len - 1, &impl->local_name )))) goto failed;
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_LOCAL_NAME, (const BYTE *)adv->name, strlen( adv->name ) ))))
            goto failed;
    }
    if (FAILED((hr = advertisement_add_uuid_sections( impl, adv )))) goto failed;
    for (i = 0; i < adv->manufacturer_data_count; i++)
    {
        const struct winebth_le_manufacturer_data *data = &adv->manufacturer_data[i];
        IBluetoothLEManufacturerData *entry;
        BYTE section[2 + WINEBTH_LE_ADV_MAX_DATA];

        if (FAILED((hr = manufacturer_data_create( data->company_id, data->data, data->size, &entry )))) goto failed;
        hr = IVector_IInspectable_Append( impl->manufacturer_data, (IInspectable *)entry );
        IBluetoothLEManufacturerData_Release( entry );
        if (FAILED(hr)) goto failed;

        section[0] = data->company_id & 0xff;
        section[1] = data->company_id >> 8;
        memcpy( &section[2], data->data, data->size );
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_MANUFACTURER, section, 2 + data->size )))) goto failed;
    }
    for (i = 0; i < adv->service_data_count; i++)
    {
        const struct winebth_le_service_data *data = &adv->service_data[i];
        BYTE section[16 + WINEBTH_LE_ADV_MAX_DATA], type;
        UINT32 value, uuid_len;

        if (uuid_short_value( &data->uuid, &value ))
        {
            uuid_len = value <= 0xffff ? 2 : 4;
            memcpy( section, &value, uuid_len );
            type = uuid_len == 2 ? AD_TYPE_SERVICE_DATA16 : AD_TYPE_SERVICE_DATA32;
        }
        else
        {
            uuid_len = 16;
            uuid_to_le_bytes( &data->uuid, section );
            type = AD_TYPE_SERVICE_DATA128;
        }
        memcpy( &section[uuid_len], data->data, data->size );
        if (FAILED((hr = advertisement_add_section( impl, type, section, uuid_len + data->size )))) goto failed;
    }
    if (adv->flags & WINEBTH_LE_ADV_FLAG_TX_POWER)
    {
        BYTE power = (BYTE)adv->tx_power;
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_TX_POWER, &power, 1 )))) goto failed;
    }
    if (adv->flags & WINEBTH_LE_ADV_FLAG_APPEARANCE)
    {
        BYTE appearance[2] = { adv->appearance & 0xff, adv->appearance >> 8 };
        if (FAILED((hr = advertisement_add_section( impl, AD_TYPE_APPEARANCE, appearance, 2 )))) goto failed;
    }

    *out = &impl->IBluetoothLEAdvertisement_iface;
    return S_OK;

failed:
    IBluetoothLEAdvertisement_Release( &impl->IBluetoothLEAdvertisement_iface );
    return hr;
}

struct adv_watcher
{
    IBluetoothLEAdvertisementWatcher IBluetoothLEAdvertisementWatcher_iface;
    LONG ref;
};

static inline struct adv_watcher *impl_from_IBluetoothLEAdvertisementWatcher( IBluetoothLEAdvertisementWatcher *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher, IBluetoothLEAdvertisementWatcher_iface );
}

static HRESULT WINAPI adv_watcher_QueryInterface( IBluetoothLEAdvertisementWatcher *iface, REFIID iid, void **out )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcher ))
    {
        IBluetoothLEAdvertisementWatcher_AddRef(( *out = &impl->IBluetoothLEAdvertisementWatcher_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_AddRef( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_watcher_Release( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI adv_watcher_GetIids( IBluetoothLEAdvertisementWatcher *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_GetRuntimeClassName( IBluetoothLEAdvertisementWatcher *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_GetTrustLevel( IBluetoothLEAdvertisementWatcher *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_get_MinSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 1000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 255000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MinOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 10000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 600000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_Status( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEAdvertisementWatcherStatus *status )
{
    FIXME( "(%p, %p): stub!\n", iface, status );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_get_ScanningMode( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEScanningMode *value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_put_ScanningMode( IBluetoothLEAdvertisementWatcher *iface, BluetoothLEScanningMode value )
{
    FIXME( "(%p, %d): stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_get_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothSignalStrengthFilter **filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_put_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothSignalStrengthFilter *filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_get_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothLEAdvertisementFilter **filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_put_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface, IBluetoothLEAdvertisementFilter *filter )
{
    FIXME( "(%p, %p): stub!\n", iface, filter );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_Start( IBluetoothLEAdvertisementWatcher *iface )
{
    FIXME( "(%p): stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_Stop( IBluetoothLEAdvertisementWatcher *iface )
{
    FIXME( "(%p): stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_add_Received( IBluetoothLEAdvertisementWatcher *iface,
                                                ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs *handler,
                                                EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_remove_Received( IBluetoothLEAdvertisementWatcher *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64x): stub!\n", iface, token.value );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_add_Stopped( IBluetoothLEAdvertisementWatcher *iface,
                                               ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs *handler,
                                               EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return E_NOTIMPL;
}


static HRESULT WINAPI adv_watcher_remove_Stopped( IBluetoothLEAdvertisementWatcher *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64x): stub!\n", iface, token.value );
    return E_NOTIMPL;
}

static const IBluetoothLEAdvertisementWatcherVtbl adv_watcher_vtbl =
{
    /* IUnknown */
    adv_watcher_QueryInterface,
    adv_watcher_AddRef,
    adv_watcher_Release,
    /* IInspectable */
    adv_watcher_GetIids,
    adv_watcher_GetRuntimeClassName,
    adv_watcher_GetTrustLevel,
    /* IBluetoothLEAdvertisementWatcher */
    adv_watcher_get_MinSamplingInternal,
    adv_watcher_get_MaxSamplingInternal,
    adv_watcher_get_MinOutOfRangeTimeout,
    adv_watcher_get_MaxOutOfRangeTimeout,
    adv_watcher_get_Status,
    adv_watcher_get_ScanningMode,
    adv_watcher_put_ScanningMode,
    adv_watcher_get_SignalStrengthFilter,
    adv_watcher_put_SignalStrengthFilter,
    adv_watcher_get_AdvertisementFilter,
    adv_watcher_put_AdvertisementFilter,
    adv_watcher_Start,
    adv_watcher_Stop,
    adv_watcher_add_Received,
    adv_watcher_remove_Received,
    adv_watcher_add_Stopped,
    adv_watcher_remove_Stopped
};

static HRESULT adv_watcher_create( IBluetoothLEAdvertisementWatcher **watcher )
{
    struct adv_watcher *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementWatcher_iface.lpVtbl = &adv_watcher_vtbl;
    impl->ref = 1;
    *watcher = &impl->IBluetoothLEAdvertisementWatcher_iface;
    return S_OK;
}

struct adv_watcher_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct adv_watcher_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher_factory, IActivationFactory_iface );
}

static HRESULT WINAPI adv_watcher_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_factory_AddRef( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_watcher_factory_Release( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI adv_watcher_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI adv_watcher_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    return adv_watcher_create( (IBluetoothLEAdvertisementWatcher **)instance );
}

static const struct IActivationFactoryVtbl adv_watcher_factory_vtbl =
{
    adv_watcher_factory_QueryInterface,
    adv_watcher_factory_AddRef,
    adv_watcher_factory_Release,
    /* IInspectable */
    adv_watcher_factory_GetIids,
    adv_watcher_factory_GetRuntimeClassName,
    adv_watcher_factory_GetTrustLevel,
    /* IActivationFactory */
    adv_watcher_factory_ActivateInstance
};

static struct adv_watcher_factory adv_watcher_factory =
{
    {&adv_watcher_factory_vtbl},
    1
};

IActivationFactory *advertisement_watcher_factory = &adv_watcher_factory.IActivationFactory_iface;
