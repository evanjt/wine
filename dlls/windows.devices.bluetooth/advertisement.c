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

#include "private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

/* --- Helpers --- */

static HRESULT class_name_string( const WCHAR *name, HSTRING *out )
{
    return WindowsCreateString( name, wcslen( name ), out );
}

/* A UUID in the Bluetooth base range that fits in 16 or 32 bits. */
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
