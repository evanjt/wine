/* IGatt* Implementation
 *
 * Copyright 2026 Vibhav Pant
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
#include "bthledef.h"
#include "bluetoothleapis.h"
#include "winioctl.h"
#include "setupapi.h"
#include "cfgmgr32.h"
#include "devpkey.h"
#include "ddk/bthguid.h"
#include "roapi.h"
#include "wine/winebth.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

/* --- Shared helpers --- */

struct event_handler
{
    IUnknown *handler;
    INT64 token;
};

struct event_handlers
{
    CRITICAL_SECTION cs;
    struct event_handler *entries;
    UINT32 count;
    UINT32 capacity;
    INT64 next_token;
};

static void event_handlers_init( struct event_handlers *handlers )
{
    memset( handlers, 0, sizeof( *handlers ) );
    InitializeCriticalSectionEx( &handlers->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
}

static void event_handlers_free( struct event_handlers *handlers )
{
    UINT32 i;
    for (i = 0; i < handlers->count; i++) IUnknown_Release( handlers->entries[i].handler );
    free( handlers->entries );
    DeleteCriticalSection( &handlers->cs );
}

static HRESULT event_handlers_add( struct event_handlers *handlers, IUnknown *handler, EventRegistrationToken *token )
{
    struct event_handler *entry;

    if (!handler) return E_INVALIDARG;
    EnterCriticalSection( &handlers->cs );
    if (handlers->count == handlers->capacity)
    {
        UINT32 capacity = max( 4, handlers->capacity * 2 );
        if (!(entry = realloc( handlers->entries, capacity * sizeof( *entry ) )))
        {
            LeaveCriticalSection( &handlers->cs );
            return E_OUTOFMEMORY;
        }
        handlers->entries = entry;
        handlers->capacity = capacity;
    }
    entry = &handlers->entries[handlers->count++];
    entry->handler = handler;
    IUnknown_AddRef( handler );
    entry->token = token->value = ++handlers->next_token;
    LeaveCriticalSection( &handlers->cs );
    return S_OK;
}

static HRESULT event_handlers_remove( struct event_handlers *handlers, EventRegistrationToken token )
{
    IUnknown *handler = NULL;
    UINT32 i;

    EnterCriticalSection( &handlers->cs );
    for (i = 0; i < handlers->count; i++)
    {
        if (handlers->entries[i].token != token.value) continue;
        handler = handlers->entries[i].handler;
        memmove( &handlers->entries[i], &handlers->entries[i + 1], (handlers->count - i - 1) * sizeof( *handlers->entries ) );
        handlers->count--;
        break;
    }
    LeaveCriticalSection( &handlers->cs );
    if (handler) IUnknown_Release( handler );
    return S_OK;
}

static HRESULT async_result_object( PROPVARIANT *result, IUnknown *object )
{
    result->vt = VT_UNKNOWN;
    result->punkVal = object;
    return S_OK;
}

/* A simple object with only IInspectable, for results and event args. */
#define DEFINE_SIMPLE_INSPECTABLE( pfx, iface_type, impl_type, class_name_str, destroy_expr )                     \
    static inline impl_type *impl_from_##iface_type( iface_type *iface )                                          \
    {                                                                                                             \
        return CONTAINING_RECORD( iface, impl_type, iface_type##_iface );                                         \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )                       \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );                                              \
        if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||                        \
            IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_##iface_type ))                       \
        {                                                                                                         \
            iface_type##_AddRef(( *out = &impl->iface_type##_iface ));                                            \
            return S_OK;                                                                                          \
        }                                                                                                         \
        *out = NULL;                                                                                              \
        FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );                          \
        return E_NOINTERFACE;                                                                                     \
    }                                                                                                             \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                                         \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        return InterlockedIncrement( &impl->ref );                                                                \
    }                                                                                                             \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                                        \
    {                                                                                                             \
        impl_type *impl = impl_from_##iface_type( iface );                                                        \
        ULONG ref = InterlockedDecrement( &impl->ref );                                                           \
        if (!ref)                                                                                                 \
        {                                                                                                         \
            destroy_expr;                                                                                         \
            free( impl );                                                                                         \
        }                                                                                                         \
        return ref;                                                                                               \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )                        \
    {                                                                                                             \
        FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );                                                 \
        return E_NOTIMPL;                                                                                         \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )                     \
    {                                                                                                             \
        return class_name_string( class_name_str, class_name );                                                   \
    }                                                                                                             \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *level )                             \
    {                                                                                                             \
        *level = BaseTrust;                                                                                       \
        return S_OK;                                                                                              \
    }

/* --- GattReadResult --- */

struct read_result
{
    IGattReadResult IGattReadResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IBuffer *value;
};

DEFINE_SIMPLE_INSPECTABLE( read_result, IGattReadResult, struct read_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult",
                           if (impl->value) IBuffer_Release( impl->value ) )

static HRESULT WINAPI read_result_get_Status( IGattReadResult *iface, GattCommunicationStatus *value )
{
    struct read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI read_result_get_Value( IGattReadResult *iface, IBuffer **value )
{
    struct read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->value)) IBuffer_AddRef( *value );
    return S_OK;
}

static const IGattReadResultVtbl read_result_vtbl =
{
    read_result_QueryInterface,
    read_result_AddRef,
    read_result_Release,
    read_result_GetIids,
    read_result_GetRuntimeClassName,
    read_result_GetTrustLevel,
    read_result_get_Status,
    read_result_get_Value,
};

HRESULT read_result_create( GattCommunicationStatus status, IBuffer *value, IGattReadResult **out )
{
    struct read_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattReadResult_iface.lpVtbl = &read_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    if ((impl->value = value)) IBuffer_AddRef( value );
    *out = &impl->IGattReadResult_iface;
    return S_OK;
}

/* --- GattWriteResult --- */

struct write_result
{
    IGattWriteResult IGattWriteResult_iface;
    LONG ref;
    GattCommunicationStatus status;
};

DEFINE_SIMPLE_INSPECTABLE( write_result, IGattWriteResult, struct write_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattWriteResult", (void)0 )

static HRESULT WINAPI write_result_get_Status( IGattWriteResult *iface, GattCommunicationStatus *value )
{
    struct write_result *impl = impl_from_IGattWriteResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI write_result_get_ProtocolError( IGattWriteResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static const IGattWriteResultVtbl write_result_vtbl =
{
    write_result_QueryInterface,
    write_result_AddRef,
    write_result_Release,
    write_result_GetIids,
    write_result_GetRuntimeClassName,
    write_result_GetTrustLevel,
    write_result_get_Status,
    write_result_get_ProtocolError,
};

HRESULT write_result_create( GattCommunicationStatus status, IGattWriteResult **out )
{
    struct write_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattWriteResult_iface.lpVtbl = &write_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    *out = &impl->IGattWriteResult_iface;
    return S_OK;
}

/* --- GattCharacteristicsResult --- */

struct characteristics_result
{
    IGattCharacteristicsResult IGattCharacteristicsResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattCharacteristic *characteristics;
};

DEFINE_SIMPLE_INSPECTABLE( characteristics_result, IGattCharacteristicsResult, struct characteristics_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult",
                           if (impl->characteristics) IVectorView_GattCharacteristic_Release( impl->characteristics ) )

static HRESULT WINAPI characteristics_result_get_Status( IGattCharacteristicsResult *iface, GattCommunicationStatus *value )
{
    struct characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI characteristics_result_get_ProtocolError( IGattCharacteristicsResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI characteristics_result_get_Characteristics( IGattCharacteristicsResult *iface,
                                                                  IVectorView_GattCharacteristic **value )
{
    struct characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->characteristics)) IVectorView_GattCharacteristic_AddRef( *value );
    return S_OK;
}

static const IGattCharacteristicsResultVtbl characteristics_result_vtbl =
{
    characteristics_result_QueryInterface,
    characteristics_result_AddRef,
    characteristics_result_Release,
    characteristics_result_GetIids,
    characteristics_result_GetRuntimeClassName,
    characteristics_result_GetTrustLevel,
    characteristics_result_get_Status,
    characteristics_result_get_ProtocolError,
    characteristics_result_get_Characteristics,
};

HRESULT characteristics_result_create( GattCommunicationStatus status, IVector_IInspectable *vector,
                                       IGattCharacteristicsResult **out )
{
    struct characteristics_result *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattCharacteristicsResult_iface.lpVtbl = &characteristics_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    if (vector && FAILED((hr = IVector_IInspectable_GetView( vector, (IVectorView_IInspectable **)&impl->characteristics ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattCharacteristicsResult_iface;
    return S_OK;
}

/* --- GattDeviceServicesResult --- */

struct services_result
{
    IGattDeviceServicesResult IGattDeviceServicesResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattDeviceService *services;
};

DEFINE_SIMPLE_INSPECTABLE( services_result, IGattDeviceServicesResult, struct services_result,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult",
                           if (impl->services) IVectorView_GattDeviceService_Release( impl->services ) )

static HRESULT WINAPI services_result_get_Status( IGattDeviceServicesResult *iface, GattCommunicationStatus *value )
{
    struct services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI services_result_get_ProtocolError( IGattDeviceServicesResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI services_result_get_Services( IGattDeviceServicesResult *iface, IVectorView_GattDeviceService **value )
{
    struct services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->services)) IVectorView_GattDeviceService_AddRef( *value );
    return S_OK;
}

static const IGattDeviceServicesResultVtbl services_result_vtbl =
{
    services_result_QueryInterface,
    services_result_AddRef,
    services_result_Release,
    services_result_GetIids,
    services_result_GetRuntimeClassName,
    services_result_GetTrustLevel,
    services_result_get_Status,
    services_result_get_ProtocolError,
    services_result_get_Services,
};

HRESULT gatt_device_services_result_create( IVector_IInspectable *services, IGattDeviceServicesResult **out )
{
    struct services_result *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattDeviceServicesResult_iface.lpVtbl = &services_result_vtbl;
    impl->ref = 1;
    impl->status = services ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable;
    if (services && FAILED((hr = IVector_IInspectable_GetView( services, (IVectorView_IInspectable **)&impl->services ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattDeviceServicesResult_iface;
    return S_OK;
}

/* --- GattValueChangedEventArgs --- */

struct value_changed_args
{
    IGattValueChangedEventArgs IGattValueChangedEventArgs_iface;
    LONG ref;
    IBuffer *value;
    DateTime timestamp;
};

DEFINE_SIMPLE_INSPECTABLE( value_changed_args, IGattValueChangedEventArgs, struct value_changed_args,
                           L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattValueChangedEventArgs",
                           if (impl->value) IBuffer_Release( impl->value ) )

static HRESULT WINAPI value_changed_args_get_CharacteristicValue( IGattValueChangedEventArgs *iface, IBuffer **value )
{
    struct value_changed_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if ((*value = impl->value)) IBuffer_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI value_changed_args_get_Timestamp( IGattValueChangedEventArgs *iface, DateTime *value )
{
    struct value_changed_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->timestamp;
    return S_OK;
}

static const IGattValueChangedEventArgsVtbl value_changed_args_vtbl =
{
    value_changed_args_QueryInterface,
    value_changed_args_AddRef,
    value_changed_args_Release,
    value_changed_args_GetIids,
    value_changed_args_GetRuntimeClassName,
    value_changed_args_GetTrustLevel,
    value_changed_args_get_CharacteristicValue,
    value_changed_args_get_Timestamp,
};

HRESULT value_changed_args_create( const BYTE *data, UINT32 size, IGattValueChangedEventArgs **out )
{
    struct value_changed_args *impl;
    FILETIME now;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattValueChangedEventArgs_iface.lpVtbl = &value_changed_args_vtbl;
    impl->ref = 1;
    GetSystemTimeAsFileTime( &now );
    impl->timestamp.UniversalTime = ((UINT64)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (FAILED((hr = buffer_create( data, size, &impl->value ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IGattValueChangedEventArgs_iface;
    return S_OK;
}

/* --- BluetoothDeviceId --- */

struct device_id
{
    IBluetoothDeviceId IBluetoothDeviceId_iface;
    LONG ref;
    HSTRING id;
};

DEFINE_SIMPLE_INSPECTABLE( device_id, IBluetoothDeviceId, struct device_id, L"Windows.Devices.Bluetooth.BluetoothDeviceId",
                           WindowsDeleteString( impl->id ) )

static HRESULT WINAPI device_id_get_Id( IBluetoothDeviceId *iface, HSTRING *value )
{
    struct device_id *impl = impl_from_IBluetoothDeviceId( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->id, value );
}

static HRESULT WINAPI device_id_get_IsClassicDevice( IBluetoothDeviceId *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI device_id_get_IsLowEnergyDevice( IBluetoothDeviceId *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static const IBluetoothDeviceIdVtbl device_id_vtbl =
{
    device_id_QueryInterface,
    device_id_AddRef,
    device_id_Release,
    device_id_GetIids,
    device_id_GetRuntimeClassName,
    device_id_GetTrustLevel,
    device_id_get_Id,
    device_id_get_IsClassicDevice,
    device_id_get_IsLowEnergyDevice,
};

HRESULT bluetoothdeviceid_create( HSTRING id, IBluetoothDeviceId **out )
{
    struct device_id *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothDeviceId_iface.lpVtbl = &device_id_vtbl;
    impl->ref = 1;
    if (FAILED((hr = WindowsDuplicateString( id, &impl->id ))))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IBluetoothDeviceId_iface;
    return S_OK;
}

struct device_id_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothDeviceIdStatics IBluetoothDeviceIdStatics_iface;
    LONG ref;
};

static inline struct device_id_statics *device_id_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct device_id_statics, IActivationFactory_iface );
}

static HRESULT WINAPI device_id_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothDeviceIdStatics ))
    {
        IActivationFactory_AddRef( iface );
        *out = &impl->IBluetoothDeviceIdStatics_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI device_id_factory_AddRef( IActivationFactory *iface )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI device_id_factory_Release( IActivationFactory *iface )
{
    struct device_id_statics *impl = device_id_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI device_id_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_id_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.BluetoothDeviceId", class_name );
}

static HRESULT WINAPI device_id_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI device_id_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl device_id_factory_vtbl =
{
    device_id_factory_QueryInterface,
    device_id_factory_AddRef,
    device_id_factory_Release,
    device_id_factory_GetIids,
    device_id_factory_GetRuntimeClassName,
    device_id_factory_GetTrustLevel,
    device_id_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( device_id_statics, IBluetoothDeviceIdStatics, struct device_id_statics, IActivationFactory_iface )

static HRESULT WINAPI device_id_statics_FromId( IBluetoothDeviceIdStatics *iface, HSTRING id, IBluetoothDeviceId **result )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_hstring( id ), result );
    return bluetoothdeviceid_create( id, result );
}

static const IBluetoothDeviceIdStaticsVtbl device_id_statics_vtbl =
{
    device_id_statics_QueryInterface,
    device_id_statics_AddRef,
    device_id_statics_Release,
    device_id_statics_GetIids,
    device_id_statics_GetRuntimeClassName,
    device_id_statics_GetTrustLevel,
    device_id_statics_FromId,
};

static struct device_id_statics device_id_statics =
{
    {&device_id_factory_vtbl},
    {&device_id_statics_vtbl},
    1
};

IActivationFactory *bluetoothdeviceid_statics_factory = &device_id_statics.IActivationFactory_iface;

/* --- GattSession --- */

struct gatt_session
{
    IGattSession IGattSession_iface;
    IClosable IClosable_iface;
    LONG ref;
    IBluetoothDeviceId *id;
    boolean maintain_connection;
    struct event_handlers pdu_changed;
    struct event_handlers status_changed;
};

static inline struct gatt_session *impl_from_IGattSession( IGattSession *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_session, IGattSession_iface );
}

static HRESULT WINAPI gatt_session_QueryInterface( IGattSession *iface, REFIID iid, void **out )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IGattSession ))
    {
        IGattSession_AddRef(( *out = &impl->IGattSession_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IGattSession_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_session_AddRef( IGattSession *iface )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_session_Release( IGattSession *iface )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IBluetoothDeviceId_Release( impl->id );
        event_handlers_free( &impl->pdu_changed );
        event_handlers_free( &impl->status_changed );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_session_GetIids( IGattSession *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_session_GetRuntimeClassName( IGattSession *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession", class_name );
}

static HRESULT WINAPI gatt_session_GetTrustLevel( IGattSession *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_DeviceId( IGattSession *iface, IBluetoothDeviceId **value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IBluetoothDeviceId_AddRef(( *value = impl->id ));
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_CanMaintainConnection( IGattSession *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI gatt_session_put_MaintainConnection( IGattSession *iface, boolean value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %d)\n", iface, value );
    impl->maintain_connection = value;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_MaintainConnection( IGattSession *iface, boolean *value )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->maintain_connection;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_MaxPduSize( IGattSession *iface, UINT16 *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = 23;
    return S_OK;
}

static HRESULT WINAPI gatt_session_get_SessionStatus( IGattSession *iface, GattSessionStatus *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    *value = GattSessionStatus_Active;
    return S_OK;
}

static HRESULT WINAPI gatt_session_add_MaxPduSizeChanged( IGattSession *iface,
                                                          ITypedEventHandler_GattSession_IInspectable *handler,
                                                          EventRegistrationToken *token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( &impl->pdu_changed, (IUnknown *)handler, token );
}

static HRESULT WINAPI gatt_session_remove_MaxPduSizeChanged( IGattSession *iface, EventRegistrationToken token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->pdu_changed, token );
}

static HRESULT WINAPI gatt_session_add_SessionStatusChanged( IGattSession *iface,
                                                             ITypedEventHandler_GattSession_GattSessionStatusChangedEventArgs *handler,
                                                             EventRegistrationToken *token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( &impl->status_changed, (IUnknown *)handler, token );
}

static HRESULT WINAPI gatt_session_remove_SessionStatusChanged( IGattSession *iface, EventRegistrationToken token )
{
    struct gatt_session *impl = impl_from_IGattSession( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->status_changed, token );
}

static const IGattSessionVtbl gatt_session_vtbl =
{
    gatt_session_QueryInterface,
    gatt_session_AddRef,
    gatt_session_Release,
    gatt_session_GetIids,
    gatt_session_GetRuntimeClassName,
    gatt_session_GetTrustLevel,
    gatt_session_get_DeviceId,
    gatt_session_get_CanMaintainConnection,
    gatt_session_put_MaintainConnection,
    gatt_session_get_MaintainConnection,
    gatt_session_get_MaxPduSize,
    gatt_session_get_SessionStatus,
    gatt_session_add_MaxPduSizeChanged,
    gatt_session_remove_MaxPduSizeChanged,
    gatt_session_add_SessionStatusChanged,
    gatt_session_remove_SessionStatusChanged,
};

DEFINE_IINSPECTABLE_( gatt_session_closable, IClosable, struct gatt_session, gatt_session_from_IClosable, IClosable_iface, &impl->IGattSession_iface )

static HRESULT WINAPI gatt_session_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl gatt_session_closable_vtbl =
{
    gatt_session_closable_QueryInterface,
    gatt_session_closable_AddRef,
    gatt_session_closable_Release,
    gatt_session_closable_GetIids,
    gatt_session_closable_GetRuntimeClassName,
    gatt_session_closable_GetTrustLevel,
    gatt_session_closable_Close,
};

HRESULT gatt_session_create( IBluetoothDeviceId *id, IGattSession **session )
{
    struct gatt_session *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattSession_iface.lpVtbl = &gatt_session_vtbl;
    impl->IClosable_iface.lpVtbl = &gatt_session_closable_vtbl;
    impl->ref = 1;
    IBluetoothDeviceId_AddRef(( impl->id = id ));
    event_handlers_init( &impl->pdu_changed );
    event_handlers_init( &impl->status_changed );
    *session = &impl->IGattSession_iface;
    return S_OK;
}

static HRESULT gatt_session_from_id_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    IBluetoothDeviceId *id;
    IGattSession *session;
    HRESULT hr;

    if (FAILED((hr = IUnknown_QueryInterface( param, &IID_IBluetoothDeviceId, (void **)&id )))) return hr;
    hr = gatt_session_create( id, &session );
    IBluetoothDeviceId_Release( id );
    if (FAILED(hr)) return hr;
    return async_result_object( result, (IUnknown *)session );
}

struct gatt_session_statics
{
    IActivationFactory IActivationFactory_iface;
    IGattSessionStatics IGattSessionStatics_iface;
    LONG ref;
};

static inline struct gatt_session_statics *gatt_session_statics_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_session_statics, IActivationFactory_iface );
}

static HRESULT WINAPI gatt_session_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattSessionStatics ))
    {
        IActivationFactory_AddRef( iface );
        *out = &impl->IGattSessionStatics_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_session_factory_AddRef( IActivationFactory *iface )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_session_factory_Release( IActivationFactory *iface )
{
    struct gatt_session_statics *impl = gatt_session_statics_from_IActivationFactory( iface );
    return InterlockedDecrement( &impl->ref );
}

static HRESULT WINAPI gatt_session_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_session_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession", class_name );
}

static HRESULT WINAPI gatt_session_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_session_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "(%p, %p): stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl gatt_session_factory_vtbl =
{
    gatt_session_factory_QueryInterface,
    gatt_session_factory_AddRef,
    gatt_session_factory_Release,
    gatt_session_factory_GetIids,
    gatt_session_factory_GetRuntimeClassName,
    gatt_session_factory_GetTrustLevel,
    gatt_session_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( gatt_session_statics, IGattSessionStatics, struct gatt_session_statics, IActivationFactory_iface )

static HRESULT WINAPI gatt_session_statics_FromDeviceIdAsync( IGattSessionStatics *iface, IBluetoothDeviceId *id,
                                                              IAsyncOperation_GattSession **async )
{
    TRACE( "(%p, %p, %p)\n", iface, id, async );
    return async_operation_inspectable_create( &IID_IAsyncOperation_GattSession, (IUnknown *)iface, (IUnknown *)id,
                                               gatt_session_from_id_async, (IAsyncOperation_IInspectable **)async );
}

static const IGattSessionStaticsVtbl gatt_session_statics_vtbl =
{
    gatt_session_statics_QueryInterface,
    gatt_session_statics_AddRef,
    gatt_session_statics_Release,
    gatt_session_statics_GetIids,
    gatt_session_statics_GetRuntimeClassName,
    gatt_session_statics_GetTrustLevel,
    gatt_session_statics_FromDeviceIdAsync,
};

static struct gatt_session_statics gatt_session_statics =
{
    {&gatt_session_factory_vtbl},
    {&gatt_session_statics_vtbl},
    1
};

IActivationFactory *gattsession_statics_factory = &gatt_session_statics.IActivationFactory_iface;

struct gatt_service
{
    IGattDeviceService IGattDeviceService_iface;
    LONG ref;

    BTH_LE_GATT_SERVICE service;
};

static inline struct gatt_service *impl_from_IGattDeviceService( IGattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_service, IGattDeviceService_iface );
}

static HRESULT WINAPI gatt_service_QueryInterface( IGattDeviceService *iface, REFIID iid, void **out )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattDeviceService ))
    {
        IGattDeviceService_AddRef(( *out = &impl->IGattDeviceService_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_service_AddRef( IGattDeviceService *iface )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_service_Release( IGattDeviceService *iface )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    if (!ref)
        free( impl );
    return ref;
}

static HRESULT WINAPI gatt_service_GetIids( IGattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetRuntimeClassName( IGattDeviceService *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetTrustLevel( IGattDeviceService *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetCharacteristics( IGattDeviceService *iface, GUID uuid, IVectorView_GattCharacteristic **chars )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), chars );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetIncludedServices( IGattDeviceService *iface, GUID uuid, IVectorView_GattDeviceService **services )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), services );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_get_DeviceId( IGattDeviceService *iface, HSTRING *value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_get_Uuid( IGattDeviceService *iface, GUID *value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );

    if (impl->service.ServiceUuid.IsShortUuid)
    {
        *value = BTH_LE_ATT_BLUETOOTH_BASE_GUID;
        value->Data1 = impl->service.ServiceUuid.Value.ShortUuid;
    }
    else
        *value = impl->service.ServiceUuid.Value.LongUuid;
    return S_OK;
}

static HRESULT WINAPI gatt_service_get_AttributeHandle( IGattDeviceService *iface, UINT16 *value )
{
    struct gatt_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->service.AttributeHandle;
    return S_OK;
}

static const IGattDeviceServiceVtbl gatt_service_vtbl =
{
    /* IUnknown */
    gatt_service_QueryInterface,
    gatt_service_AddRef,
    gatt_service_Release,
    /* IInspectable */
    gatt_service_GetIids,
    gatt_service_GetRuntimeClassName,
    gatt_service_GetTrustLevel,
    /* IGattDeviceService */
    gatt_service_GetCharacteristics,
    gatt_service_GetIncludedServices,
    gatt_service_get_DeviceId,
    gatt_service_get_Uuid,
    gatt_service_get_AttributeHandle
};

HRESULT gatt_service_create( const BTH_LE_GATT_SERVICE *svc, IGattDeviceService **service )
{
    struct gatt_service *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) )))
        return E_OUTOFMEMORY;
    impl->IGattDeviceService_iface.lpVtbl = &gatt_service_vtbl;
    impl->ref = 1;
    impl->service = *svc;
    *service = &impl->IGattDeviceService_iface;
    return S_OK;
}
