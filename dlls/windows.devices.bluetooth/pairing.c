/* DeviceInformationPairing Implementation
 *
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

/* How long a PairingRequested handler may hold a deferral before the request is refused. */
#define PAIRING_DEFERRAL_TIMEOUT 60000

struct pairing_result
{
    IDevicePairingResult IDevicePairingResult_iface;
    LONG ref;
    DevicePairingResultStatus status;
    DevicePairingProtectionLevel level;
};

static inline struct pairing_result *impl_from_IDevicePairingResult( IDevicePairingResult *iface )
{
    return CONTAINING_RECORD( iface, struct pairing_result, IDevicePairingResult_iface );
}

static HRESULT WINAPI pairing_result_QueryInterface( IDevicePairingResult *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDevicePairingResult ))
    {
        *out = iface;
        IDevicePairingResult_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI pairing_result_AddRef( IDevicePairingResult *iface )
{
    struct pairing_result *impl = impl_from_IDevicePairingResult( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI pairing_result_Release( IDevicePairingResult *iface )
{
    struct pairing_result *impl = impl_from_IDevicePairingResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI pairing_result_GetIids( IDevicePairingResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI pairing_result_GetRuntimeClassName( IDevicePairingResult *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DevicePairingResult", class_name );
}

static HRESULT WINAPI pairing_result_GetTrustLevel( IDevicePairingResult *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pairing_result_get_Status( IDevicePairingResult *iface, DevicePairingResultStatus *value )
{
    struct pairing_result *impl = impl_from_IDevicePairingResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI pairing_result_get_ProtectionLevelUsed( IDevicePairingResult *iface, DevicePairingProtectionLevel *value )
{
    struct pairing_result *impl = impl_from_IDevicePairingResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->level;
    return S_OK;
}

static const IDevicePairingResultVtbl pairing_result_vtbl =
{
    pairing_result_QueryInterface,
    pairing_result_AddRef,
    pairing_result_Release,
    pairing_result_GetIids,
    pairing_result_GetRuntimeClassName,
    pairing_result_GetTrustLevel,
    pairing_result_get_Status,
    pairing_result_get_ProtectionLevelUsed,
};

static HRESULT pairing_result_create( DevicePairingResultStatus status, IDevicePairingResult **out )
{
    struct pairing_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IDevicePairingResult_iface.lpVtbl = &pairing_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    impl->level = status == DevicePairingResultStatus_Paired || status == DevicePairingResultStatus_AlreadyPaired
                      ? DevicePairingProtectionLevel_Encryption : DevicePairingProtectionLevel_None;
    *out = &impl->IDevicePairingResult_iface;
    return S_OK;
}

struct unpairing_result
{
    IDeviceUnpairingResult IDeviceUnpairingResult_iface;
    LONG ref;
    DeviceUnpairingResultStatus status;
};

static inline struct unpairing_result *impl_from_IDeviceUnpairingResult( IDeviceUnpairingResult *iface )
{
    return CONTAINING_RECORD( iface, struct unpairing_result, IDeviceUnpairingResult_iface );
}

static HRESULT WINAPI unpairing_result_QueryInterface( IDeviceUnpairingResult *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDeviceUnpairingResult ))
    {
        *out = iface;
        IDeviceUnpairingResult_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI unpairing_result_AddRef( IDeviceUnpairingResult *iface )
{
    struct unpairing_result *impl = impl_from_IDeviceUnpairingResult( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI unpairing_result_Release( IDeviceUnpairingResult *iface )
{
    struct unpairing_result *impl = impl_from_IDeviceUnpairingResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI unpairing_result_GetIids( IDeviceUnpairingResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI unpairing_result_GetRuntimeClassName( IDeviceUnpairingResult *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DeviceUnpairingResult", class_name );
}

static HRESULT WINAPI unpairing_result_GetTrustLevel( IDeviceUnpairingResult *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI unpairing_result_get_Status( IDeviceUnpairingResult *iface, DeviceUnpairingResultStatus *value )
{
    struct unpairing_result *impl = impl_from_IDeviceUnpairingResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->status;
    return S_OK;
}

static const IDeviceUnpairingResultVtbl unpairing_result_vtbl =
{
    unpairing_result_QueryInterface,
    unpairing_result_AddRef,
    unpairing_result_Release,
    unpairing_result_GetIids,
    unpairing_result_GetRuntimeClassName,
    unpairing_result_GetTrustLevel,
    unpairing_result_get_Status,
};

static HRESULT unpairing_result_create( DeviceUnpairingResultStatus status, IDeviceUnpairingResult **out )
{
    struct unpairing_result *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IDeviceUnpairingResult_iface.lpVtbl = &unpairing_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    *out = &impl->IDeviceUnpairingResult_iface;
    return S_OK;
}

/* The arguments handed to PairingRequested handlers. The handler answers with Accept, AcceptWithPin or by
 * finishing a deferral, and the auth callback that raised the event reads the answer afterwards. */
struct pairing_request_args
{
    IDevicePairingRequestedEventArgs IDevicePairingRequestedEventArgs_iface;
    LONG ref;
    IDeviceInformation *info;
    DevicePairingKinds kind;
    HSTRING pin;

    CRITICAL_SECTION cs;
    BOOL accepted;
    UINT32 passkey; /* Guarded by cs */
    HANDLE deferral_done; /* Guarded by cs, created by the first GetDeferral */
};

static inline struct pairing_request_args *impl_from_IDevicePairingRequestedEventArgs( IDevicePairingRequestedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct pairing_request_args, IDevicePairingRequestedEventArgs_iface );
}

struct deferral
{
    IDeferral IDeferral_iface;
    IClosable IClosable_iface;
    LONG ref;
    struct pairing_request_args *args;
};

static inline struct deferral *impl_from_IDeferral( IDeferral *iface )
{
    return CONTAINING_RECORD( iface, struct deferral, IDeferral_iface );
}

static HRESULT WINAPI deferral_QueryInterface( IDeferral *iface, REFIID iid, void **out )
{
    struct deferral *impl = impl_from_IDeferral( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDeferral ))
    {
        *out = iface;
        IDeferral_AddRef( iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IDeferral_AddRef( iface );
        *out = &impl->IClosable_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI deferral_AddRef( IDeferral *iface )
{
    struct deferral *impl = impl_from_IDeferral( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI deferral_Release( IDeferral *iface )
{
    struct deferral *impl = impl_from_IDeferral( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IDevicePairingRequestedEventArgs_Release( &impl->args->IDevicePairingRequestedEventArgs_iface );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI deferral_GetIids( IDeferral *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI deferral_GetRuntimeClassName( IDeferral *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Foundation.Deferral", class_name );
}

static HRESULT WINAPI deferral_GetTrustLevel( IDeferral *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI deferral_Complete( IDeferral *iface )
{
    struct deferral *impl = impl_from_IDeferral( iface );
    TRACE( "(%p)\n", iface );
    SetEvent( impl->args->deferral_done );
    return S_OK;
}

static const IDeferralVtbl deferral_vtbl =
{
    deferral_QueryInterface,
    deferral_AddRef,
    deferral_Release,
    deferral_GetIids,
    deferral_GetRuntimeClassName,
    deferral_GetTrustLevel,
    deferral_Complete,
};

DEFINE_IINSPECTABLE( deferral_closable, IClosable, struct deferral, IDeferral_iface )

static HRESULT WINAPI deferral_closable_Close( IClosable *iface )
{
    struct deferral *impl = impl_from_IClosable( iface );
    TRACE( "(%p)\n", iface );
    return IDeferral_Complete( &impl->IDeferral_iface );
}

static const IClosableVtbl deferral_closable_vtbl =
{
    deferral_closable_QueryInterface,
    deferral_closable_AddRef,
    deferral_closable_Release,
    deferral_closable_GetIids,
    deferral_closable_GetRuntimeClassName,
    deferral_closable_GetTrustLevel,
    deferral_closable_Close,
};

static HRESULT WINAPI pairing_request_args_QueryInterface( IDevicePairingRequestedEventArgs *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDevicePairingRequestedEventArgs ))
    {
        *out = iface;
        IDevicePairingRequestedEventArgs_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI pairing_request_args_AddRef( IDevicePairingRequestedEventArgs *iface )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI pairing_request_args_Release( IDevicePairingRequestedEventArgs *iface )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        if (impl->deferral_done) CloseHandle( impl->deferral_done );
        IDeviceInformation_Release( impl->info );
        WindowsDeleteString( impl->pin );
        DeleteCriticalSection( &impl->cs );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI pairing_request_args_GetIids( IDevicePairingRequestedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI pairing_request_args_GetRuntimeClassName( IDevicePairingRequestedEventArgs *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DevicePairingRequestedEventArgs", class_name );
}

static HRESULT WINAPI pairing_request_args_GetTrustLevel( IDevicePairingRequestedEventArgs *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI pairing_request_args_get_DeviceInformation( IDevicePairingRequestedEventArgs *iface, IDeviceInformation **value )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IDeviceInformation_AddRef(( *value = impl->info ));
    return S_OK;
}

static HRESULT WINAPI pairing_request_args_get_PairingKind( IDevicePairingRequestedEventArgs *iface, DevicePairingKinds *value )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = impl->kind;
    return S_OK;
}

static HRESULT WINAPI pairing_request_args_get_Pin( IDevicePairingRequestedEventArgs *iface, HSTRING *value )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return WindowsDuplicateString( impl->pin, value );
}

static HRESULT WINAPI pairing_request_args_Accept( IDevicePairingRequestedEventArgs *iface )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );

    TRACE( "(%p)\n", iface );

    EnterCriticalSection( &impl->cs );
    impl->accepted = TRUE;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI pairing_request_args_AcceptWithPin( IDevicePairingRequestedEventArgs *iface, HSTRING pin )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    const WCHAR *digits = WindowsGetStringRawBuffer( pin, NULL );
    WCHAR *end;
    UINT32 passkey;

    TRACE( "(%p, %s)\n", iface, debugstr_hstring( pin ) );

    passkey = wcstoul( digits, &end, 10 );
    if (end == digits || *end || passkey > 999999) return E_INVALIDARG;
    EnterCriticalSection( &impl->cs );
    impl->accepted = TRUE;
    impl->passkey = passkey;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI pairing_request_args_GetDeferral( IDevicePairingRequestedEventArgs *iface, IDeferral **result )
{
    struct pairing_request_args *impl = impl_from_IDevicePairingRequestedEventArgs( iface );
    struct deferral *deferral;

    TRACE( "(%p, %p)\n", iface, result );

    EnterCriticalSection( &impl->cs );
    if (!impl->deferral_done && !(impl->deferral_done = CreateEventW( NULL, TRUE, FALSE, NULL )))
    {
        LeaveCriticalSection( &impl->cs );
        return HRESULT_FROM_WIN32( GetLastError() );
    }
    LeaveCriticalSection( &impl->cs );

    if (!(deferral = calloc( 1, sizeof( *deferral ) ))) return E_OUTOFMEMORY;
    deferral->IDeferral_iface.lpVtbl = &deferral_vtbl;
    deferral->IClosable_iface.lpVtbl = &deferral_closable_vtbl;
    deferral->ref = 1;
    deferral->args = impl;
    IDevicePairingRequestedEventArgs_AddRef( iface );
    *result = &deferral->IDeferral_iface;
    return S_OK;
}

static const IDevicePairingRequestedEventArgsVtbl pairing_request_args_vtbl =
{
    pairing_request_args_QueryInterface,
    pairing_request_args_AddRef,
    pairing_request_args_Release,
    pairing_request_args_GetIids,
    pairing_request_args_GetRuntimeClassName,
    pairing_request_args_GetTrustLevel,
    pairing_request_args_get_DeviceInformation,
    pairing_request_args_get_PairingKind,
    pairing_request_args_get_Pin,
    pairing_request_args_Accept,
    pairing_request_args_AcceptWithPin,
    pairing_request_args_GetDeferral,
};

static HRESULT pairing_request_args_create( IDeviceInformation *info, DevicePairingKinds kind, const WCHAR *pin,
                                            struct pairing_request_args **out )
{
    struct pairing_request_args *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IDevicePairingRequestedEventArgs_iface.lpVtbl = &pairing_request_args_vtbl;
    impl->ref = 1;
    impl->kind = kind;
    if (pin && FAILED(hr = WindowsCreateString( pin, wcslen( pin ), &impl->pin )))
    {
        free( impl );
        return hr;
    }
    IDeviceInformation_AddRef(( impl->info = info ));
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    *out = impl;
    return S_OK;
}

struct device_pairing
{
    IDeviceInformationPairing IDeviceInformationPairing_iface;
    IDeviceInformationPairing2 IDeviceInformationPairing2_iface;
    IDeviceInformationCustomPairing IDeviceInformationCustomPairing_iface;
    LONG ref;
    HSTRING id;
    HSTRING name;
    UINT64 addr;
    struct event_handlers pairing_requested;
};

static inline struct device_pairing *impl_from_IDeviceInformationPairing( IDeviceInformationPairing *iface )
{
    return CONTAINING_RECORD( iface, struct device_pairing, IDeviceInformationPairing_iface );
}

/* One PairAsync call. The auth callback records why the ceremony was refused so the result can say so. */
struct pairing_session
{
    IUnknown IUnknown_iface;
    LONG ref;
    struct device_pairing *pairing;
    DevicePairingKinds kinds;
    BOOL custom;
    DevicePairingResultStatus refused;
};

static inline struct pairing_session *impl_from_IUnknown( IUnknown *iface )
{
    return CONTAINING_RECORD( iface, struct pairing_session, IUnknown_iface );
}

static HRESULT WINAPI pairing_session_QueryInterface( IUnknown *iface, REFIID iid, void **out )
{
    if (IsEqualGUID( iid, &IID_IUnknown ))
    {
        *out = iface;
        IUnknown_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI pairing_session_AddRef( IUnknown *iface )
{
    struct pairing_session *impl = impl_from_IUnknown( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI pairing_session_Release( IUnknown *iface )
{
    struct pairing_session *impl = impl_from_IUnknown( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        IDeviceInformationPairing_Release( &impl->pairing->IDeviceInformationPairing_iface );
        free( impl );
    }
    return ref;
}

static const IUnknownVtbl pairing_session_vtbl =
{
    pairing_session_QueryInterface,
    pairing_session_AddRef,
    pairing_session_Release,
};

static BOOL device_paired( UINT64 addr )
{
    BLUETOOTH_FIND_RADIO_PARAMS params = { .dwSize = sizeof( params ) };
    BLUETOOTH_DEVICE_INFO info = { .dwSize = sizeof( info ), .Address.ullLong = addr };
    HBLUETOOTH_RADIO_FIND find;
    BOOL paired = FALSE;
    HANDLE radio;

    if (!(find = BluetoothFindFirstRadio( &params, &radio ))) return FALSE;
    do
    {
        paired = !BluetoothGetDeviceInfo( radio, &info ) && info.fAuthenticated;
        CloseHandle( radio );
    } while (!paired && BluetoothFindNextRadio( find, &radio ));
    BluetoothFindRadioClose( find );
    return paired;
}

static DevicePairingResultStatus pairing_status_from_error( DWORD error )
{
    switch (error)
    {
    case ERROR_SUCCESS: return DevicePairingResultStatus_Paired;
    case ERROR_NO_MORE_ITEMS: return DevicePairingResultStatus_AlreadyPaired;
    case ERROR_OPERATION_ABORTED: return DevicePairingResultStatus_PairingCanceled;
    case ERROR_TIMEOUT:
    case WAIT_TIMEOUT: return DevicePairingResultStatus_AuthenticationTimeout;
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_BAD_UNIT: return DevicePairingResultStatus_ConnectionRejected;
    case ERROR_OPERATION_IN_PROGRESS:
    case ERROR_BUSY: return DevicePairingResultStatus_OperationAlreadyInProgress;
    case ERROR_INTERNAL_ERROR:
    case ERROR_NOT_AUTHENTICATED: return DevicePairingResultStatus_AuthenticationFailure;
    case ERROR_ACCESS_DENIED: return DevicePairingResultStatus_AccessDenied;
    default: return DevicePairingResultStatus_Failed;
    }
}

/* Called by bluetoothapis on its own thread while BluetoothAuthenticateDeviceEx blocks the async thread. */
static BOOL CALLBACK pairing_auth_callback( void *ctx, BLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS *params )
{
    struct pairing_session *session = ctx;
    struct device_pairing *pairing = session->pairing;
    BLUETOOTH_AUTHENTICATE_RESPONSE response = { .authMethod = params->authenticationMethod, .negativeResponse = TRUE };
    struct pairing_request_args *args = NULL;
    IDeviceInformation *info = NULL;
    DevicePairingKinds kind;
    IUnknown **handlers;
    UINT32 i, count;
    WCHAR pin[7];
    DWORD ret;

    TRACE( "(%p, %p) method %d\n", ctx, params, params->authenticationMethod );

    response.bthAddressRemote.ullLong = pairing->addr;
    switch (params->authenticationMethod)
    {
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
        kind = DevicePairingKinds_ConfirmPinMatch;
        response.numericCompInfo.NumericValue = params->Numeric_Value;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
        kind = DevicePairingKinds_DisplayPin;
        response.passkeyInfo.passkey = params->Passkey;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        kind = DevicePairingKinds_ProvidePin;
        break;
    default:
        FIXME( "Unsupported authentication method %d\n", params->authenticationMethod );
        kind = DevicePairingKinds_None;
        break;
    }
    swprintf( pin, ARRAY_SIZE( pin ), L"%06u", params->Passkey );

    if (!(session->kinds & kind))
    {
        WARN( "Pairing kind %#x was not offered, refusing\n", kind );
        session->refused = DevicePairingResultStatus_RequiredHandlerNotRegistered;
    }
    else if (FAILED(device_information_create( pairing->id, pairing->name, pairing->addr, &info )) ||
             FAILED(pairing_request_args_create( info, kind, kind == DevicePairingKinds_ProvidePin ? NULL : pin, &args )))
        session->refused = DevicePairingResultStatus_Failed;
    else
    {
        count = event_handlers_snapshot( &pairing->pairing_requested, &handlers );
        for (i = 0; i < count; i++)
        {
            ITypedEventHandler_DeviceInformationCustomPairing_DevicePairingRequestedEventArgs_Invoke(
                (ITypedEventHandler_DeviceInformationCustomPairing_DevicePairingRequestedEventArgs *)handlers[i],
                &pairing->IDeviceInformationCustomPairing_iface, &args->IDevicePairingRequestedEventArgs_iface );
            IUnknown_Release( handlers[i] );
        }
        free( handlers );

        EnterCriticalSection( &args->cs );
        if (args->deferral_done && !args->accepted)
        {
            LeaveCriticalSection( &args->cs );
            if (WaitForSingleObject( args->deferral_done, PAIRING_DEFERRAL_TIMEOUT ) == WAIT_TIMEOUT)
                WARN( "PairingRequested deferral timed out\n" );
            EnterCriticalSection( &args->cs );
        }
        response.negativeResponse = !args->accepted;
        if (kind == DevicePairingKinds_ProvidePin) response.passkeyInfo.passkey = args->passkey;
        LeaveCriticalSection( &args->cs );
        if (!args->accepted) session->refused = DevicePairingResultStatus_RejectedByHandler;
    }

    if ((ret = BluetoothSendAuthenticationResponseEx( NULL, &response )))
        WARN( "BluetoothSendAuthenticationResponseEx failed: %lu\n", ret );
    if (args) IDevicePairingRequestedEventArgs_Release( &args->IDevicePairingRequestedEventArgs_iface );
    if (info) IDeviceInformation_Release( info );
    return TRUE;
}

static HRESULT pairing_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct pairing_session *session = impl_from_IUnknown( param );
    BLUETOOTH_DEVICE_INFO info = { .dwSize = sizeof( info ), .Address.ullLong = session->pairing->addr };
    HBLUETOOTH_AUTHENTICATION_REGISTRATION registration = NULL;
    DevicePairingResultStatus status;
    IDevicePairingResult *pairing_result;
    HRESULT hr;
    DWORD ret;

    if (!called_async) return STATUS_PENDING;

    if (device_paired( info.Address.ullLong )) status = DevicePairingResultStatus_AlreadyPaired;
    else if (session->custom && !session->pairing->pairing_requested.count)
        status = DevicePairingResultStatus_RequiredHandlerNotRegistered;
    else if (session->custom &&
             (ret = BluetoothRegisterForAuthenticationEx( &info, &registration, pairing_auth_callback, session )))
    {
        WARN( "BluetoothRegisterForAuthenticationEx failed: %lu\n", ret );
        status = DevicePairingResultStatus_Failed;
    }
    else
    {
        ret = BluetoothAuthenticateDeviceEx( NULL, NULL, &info, NULL, MITMProtectionNotRequired );
        if (registration) BluetoothUnregisterAuthentication( registration );
        if (ret && session->refused != DevicePairingResultStatus_Paired) status = session->refused;
        else status = pairing_status_from_error( ret );
        TRACE( "BluetoothAuthenticateDeviceEx returned %lu, status %d\n", ret, status );
    }

    if (FAILED(hr = pairing_result_create( status, &pairing_result ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)pairing_result;
    return S_OK;
}

static HRESULT pairing_start( struct device_pairing *pairing, DevicePairingKinds kinds, BOOL custom,
                              IAsyncOperation_DevicePairingResult **async )
{
    struct pairing_session *session;
    HRESULT hr;

    if (custom && !kinds) return E_INVALIDARG;
    if (!(session = calloc( 1, sizeof( *session ) ))) return E_OUTOFMEMORY;
    session->IUnknown_iface.lpVtbl = &pairing_session_vtbl;
    session->ref = 1;
    session->kinds = kinds;
    session->custom = custom;
    session->refused = DevicePairingResultStatus_Paired;
    session->pairing = pairing;
    IDeviceInformationPairing_AddRef( &pairing->IDeviceInformationPairing_iface );

    hr = async_operation_inspectable_create( &IID_IAsyncOperation_DevicePairingResult,
                                             (IUnknown *)&pairing->IDeviceInformationPairing_iface, &session->IUnknown_iface,
                                             pairing_async, (IAsyncOperation_IInspectable **)async );
    IUnknown_Release( &session->IUnknown_iface );
    return hr;
}

static HRESULT WINAPI device_pairing_QueryInterface( IDeviceInformationPairing *iface, REFIID iid, void **out )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDeviceInformationPairing ))
    {
        *out = iface;
        IDeviceInformationPairing_AddRef( iface );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IDeviceInformationPairing2 ))
    {
        IDeviceInformationPairing_AddRef( iface );
        *out = &impl->IDeviceInformationPairing2_iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI device_pairing_AddRef( IDeviceInformationPairing *iface )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI device_pairing_Release( IDeviceInformationPairing *iface )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        event_handlers_free( &impl->pairing_requested );
        WindowsDeleteString( impl->id );
        WindowsDeleteString( impl->name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI device_pairing_GetIids( IDeviceInformationPairing *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_pairing_GetRuntimeClassName( IDeviceInformationPairing *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DeviceInformationPairing", class_name );
}

static HRESULT WINAPI device_pairing_GetTrustLevel( IDeviceInformationPairing *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI device_pairing_get_IsPaired( IDeviceInformationPairing *iface, boolean *value )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = device_paired( impl->addr );
    return S_OK;
}

static HRESULT WINAPI device_pairing_get_CanPair( IDeviceInformationPairing *iface, boolean *value )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = !device_paired( impl->addr );
    return S_OK;
}

static HRESULT WINAPI device_pairing_PairAsync( IDeviceInformationPairing *iface, IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    TRACE( "(%p, %p)\n", iface, result );
    return pairing_start( impl, DevicePairingKinds_ConfirmOnly, FALSE, result );
}

static HRESULT WINAPI device_pairing_PairWithProtectionLevelAsync( IDeviceInformationPairing *iface,
                                                                   DevicePairingProtectionLevel level,
                                                                   IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( iface );
    TRACE( "(%p, %d, %p)\n", iface, level, result );
    return pairing_start( impl, DevicePairingKinds_ConfirmOnly, FALSE, result );
}

static const IDeviceInformationPairingVtbl device_pairing_vtbl =
{
    device_pairing_QueryInterface,
    device_pairing_AddRef,
    device_pairing_Release,
    device_pairing_GetIids,
    device_pairing_GetRuntimeClassName,
    device_pairing_GetTrustLevel,
    device_pairing_get_IsPaired,
    device_pairing_get_CanPair,
    device_pairing_PairAsync,
    device_pairing_PairWithProtectionLevelAsync,
};

DEFINE_IINSPECTABLE( device_pairing2, IDeviceInformationPairing2, struct device_pairing, IDeviceInformationPairing_iface )

static HRESULT WINAPI device_pairing2_get_ProtectionLevel( IDeviceInformationPairing2 *iface, DevicePairingProtectionLevel *value )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    *value = device_paired( impl->addr ) ? DevicePairingProtectionLevel_Encryption : DevicePairingProtectionLevel_None;
    return S_OK;
}

static HRESULT WINAPI device_pairing2_get_Custom( IDeviceInformationPairing2 *iface, IDeviceInformationCustomPairing **value )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    IDeviceInformationCustomPairing_AddRef(( *value = &impl->IDeviceInformationCustomPairing_iface ));
    return S_OK;
}

static HRESULT WINAPI device_pairing2_PairWithProtectionLevelAndSettingsAsync( IDeviceInformationPairing2 *iface,
                                                                               DevicePairingProtectionLevel level,
                                                                               IDevicePairingSettings *settings,
                                                                               IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing2( iface );
    TRACE( "(%p, %d, %p, %p)\n", iface, level, settings, result );
    return pairing_start( impl, DevicePairingKinds_ConfirmOnly, FALSE, result );
}

static HRESULT unpairing_async( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing( (IDeviceInformationPairing *)invoker );
    BLUETOOTH_ADDRESS addr = { .ullLong = impl->addr };
    DeviceUnpairingResultStatus status;
    IDeviceUnpairingResult *unpairing_result;
    HRESULT hr;
    DWORD ret;

    if (!called_async) return STATUS_PENDING;

    switch ((ret = BluetoothRemoveDevice( &addr )))
    {
    case ERROR_SUCCESS: status = DeviceUnpairingResultStatus_Unpaired; break;
    case ERROR_NOT_FOUND: status = DeviceUnpairingResultStatus_AlreadyUnpaired; break;
    case ERROR_ACCESS_DENIED: status = DeviceUnpairingResultStatus_AccessDenied; break;
    default: status = DeviceUnpairingResultStatus_Failed; break;
    }
    TRACE( "BluetoothRemoveDevice returned %lu, status %d\n", ret, status );

    if (FAILED(hr = unpairing_result_create( status, &unpairing_result ))) return hr;
    result->vt = VT_UNKNOWN;
    result->punkVal = (IUnknown *)unpairing_result;
    return S_OK;
}

static HRESULT WINAPI device_pairing2_UnpairAsync( IDeviceInformationPairing2 *iface, IAsyncOperation_DeviceUnpairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationPairing2( iface );
    TRACE( "(%p, %p)\n", iface, result );
    return async_operation_inspectable_create( &IID_IAsyncOperation_DeviceUnpairingResult,
                                               (IUnknown *)&impl->IDeviceInformationPairing_iface, NULL, unpairing_async,
                                               (IAsyncOperation_IInspectable **)result );
}

static const IDeviceInformationPairing2Vtbl device_pairing2_vtbl =
{
    device_pairing2_QueryInterface,
    device_pairing2_AddRef,
    device_pairing2_Release,
    device_pairing2_GetIids,
    device_pairing2_GetRuntimeClassName,
    device_pairing2_GetTrustLevel,
    device_pairing2_get_ProtectionLevel,
    device_pairing2_get_Custom,
    device_pairing2_PairWithProtectionLevelAndSettingsAsync,
    device_pairing2_UnpairAsync,
};

/* DeviceInformationCustomPairing shares the pairing object's lifetime but is its own runtime class. */
static inline struct device_pairing *impl_from_IDeviceInformationCustomPairing( IDeviceInformationCustomPairing *iface )
{
    return CONTAINING_RECORD( iface, struct device_pairing, IDeviceInformationCustomPairing_iface );
}

static HRESULT WINAPI custom_pairing_QueryInterface( IDeviceInformationCustomPairing *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IDeviceInformationCustomPairing ))
    {
        *out = iface;
        IDeviceInformationCustomPairing_AddRef( iface );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI custom_pairing_AddRef( IDeviceInformationCustomPairing *iface )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    return IDeviceInformationPairing_AddRef( &impl->IDeviceInformationPairing_iface );
}

static ULONG WINAPI custom_pairing_Release( IDeviceInformationCustomPairing *iface )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    return IDeviceInformationPairing_Release( &impl->IDeviceInformationPairing_iface );
}

static HRESULT WINAPI custom_pairing_GetIids( IDeviceInformationCustomPairing *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI custom_pairing_GetRuntimeClassName( IDeviceInformationCustomPairing *iface, HSTRING *class_name )
{
    return class_name_string( L"Windows.Devices.Enumeration.DeviceInformationCustomPairing", class_name );
}

static HRESULT WINAPI custom_pairing_GetTrustLevel( IDeviceInformationCustomPairing *iface, TrustLevel *level )
{
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI custom_pairing_PairAsync( IDeviceInformationCustomPairing *iface, DevicePairingKinds kinds,
                                                IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    TRACE( "(%p, %#x, %p)\n", iface, kinds, result );
    return pairing_start( impl, kinds, TRUE, result );
}

static HRESULT WINAPI custom_pairing_PairWithProtectionLevelAsync( IDeviceInformationCustomPairing *iface,
                                                                   DevicePairingKinds kinds,
                                                                   DevicePairingProtectionLevel level,
                                                                   IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    TRACE( "(%p, %#x, %d, %p)\n", iface, kinds, level, result );
    return pairing_start( impl, kinds, TRUE, result );
}

static HRESULT WINAPI custom_pairing_PairWithProtectionLevelAndSettingsAsync( IDeviceInformationCustomPairing *iface,
                                                                              DevicePairingKinds kinds,
                                                                              DevicePairingProtectionLevel level,
                                                                              IDevicePairingSettings *settings,
                                                                              IAsyncOperation_DevicePairingResult **result )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    TRACE( "(%p, %#x, %d, %p, %p)\n", iface, kinds, level, settings, result );
    return pairing_start( impl, kinds, TRUE, result );
}

static HRESULT WINAPI custom_pairing_add_PairingRequested( IDeviceInformationCustomPairing *iface,
                                                           ITypedEventHandler_DeviceInformationCustomPairing_DevicePairingRequestedEventArgs *handler,
                                                           EventRegistrationToken *token )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    return event_handlers_add( &impl->pairing_requested, (IUnknown *)handler, token );
}

static HRESULT WINAPI custom_pairing_remove_PairingRequested( IDeviceInformationCustomPairing *iface, EventRegistrationToken token )
{
    struct device_pairing *impl = impl_from_IDeviceInformationCustomPairing( iface );
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return event_handlers_remove( &impl->pairing_requested, token );
}

static const IDeviceInformationCustomPairingVtbl custom_pairing_vtbl =
{
    custom_pairing_QueryInterface,
    custom_pairing_AddRef,
    custom_pairing_Release,
    custom_pairing_GetIids,
    custom_pairing_GetRuntimeClassName,
    custom_pairing_GetTrustLevel,
    custom_pairing_PairAsync,
    custom_pairing_PairWithProtectionLevelAsync,
    custom_pairing_PairWithProtectionLevelAndSettingsAsync,
    custom_pairing_add_PairingRequested,
    custom_pairing_remove_PairingRequested,
};

HRESULT device_pairing_create( HSTRING id, HSTRING name, UINT64 addr, IDeviceInformationPairing **out )
{
    struct device_pairing *impl;
    HRESULT hr;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IDeviceInformationPairing_iface.lpVtbl = &device_pairing_vtbl;
    impl->IDeviceInformationPairing2_iface.lpVtbl = &device_pairing2_vtbl;
    impl->IDeviceInformationCustomPairing_iface.lpVtbl = &custom_pairing_vtbl;
    impl->ref = 1;
    impl->addr = addr;
    if (FAILED(hr = WindowsDuplicateString( id, &impl->id )) || FAILED(hr = WindowsDuplicateString( name, &impl->name )))
    {
        WindowsDeleteString( impl->id );
        free( impl );
        return hr;
    }
    event_handlers_init( &impl->pairing_requested );
    *out = &impl->IDeviceInformationPairing_iface;
    return S_OK;
}
