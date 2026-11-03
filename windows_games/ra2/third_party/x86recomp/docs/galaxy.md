# GOG Galaxy SDK (Galaxy64.dll): observed behaviour

What runtime/hle/galaxy64.cpp answers, and how the parts it answers were
identified. The SDK's headers are not used; interface methods are known
by vtable slot from the calls a client makes, listener types from the
RTTI of the client's listener classes. The build:

| File | Version | SHA-256 |
|---|---|---|
| Galaxy64.dll | 1.152.4.0 | 56d7e97c29564d6c64bd9455ea90dcf5bf1e3863432d342e3d81d730e6c3ea5a |

The HLE stands for a PC without the Galaxy service: Init succeeds,
nothing ever signs in.

## Interfaces
The accessors (`galaxy::api::User()`, `Apps()`, `ListenerRegistrar()`,
...) return interface pointers; clients call them twice per use (a null
check, then the call). Each MSVC vtable starts with the virtual
destructor in slot 0.

- IListenerRegistrar [1] Register(ListenerType, IGalaxyListener*), [2]
  Unregister(ListenerType, IGalaxyListener*): the calls made from the
  constructor and destructor of `SelfRegisteringListener<I, ...>`.
- IUser [7] SignInGalaxy(bool requireOnline, IAuthListener*): called as
  (false, NULL) with r9 left holding `this`, so it takes two arguments
  (the form without a timeout).
- IApps [5]: (ProductID = 0) -> const char*, a current-language query
  (GetCurrentGameLanguage or its language-code form; the call shape does
  not tell them apart). The client checks for NULL.

## Listener types
From the RTTI of `SelfRegisteringListener<I, &ListenerRegistrar>` and
the type each one's constructor registers:

| Type | Interface |
|---|---|
| 7 | IAuthListener |
| 17 | IPersonaDataChangedListener |
| 38 | IOverlayVisibilityChangeListener |
| 39 | IOverlayInitializationStateChangeListener |

IAuthListener: [1] OnAuthSuccess(), [2] OnAuthFailure(FailureReason),
[3] OnAuthLost() (three pure slots in the base vtable; a client's
derived class fills them).

FailureReason, from a client's OnAuthFailure switching over 0-7 with a
message for each: 0 undefined, 1 Galaxy service not installed or not
starting, 2 Galaxy service not signed in, 3 backend unreachable, 4 no
license for the application, 5 invalid credentials or token, 6 Galaxy
not initialized, 7 external service unreachable.

Offline, SignInGalaxy without a listener queues OnAuthFailure(1) for
the registered type-7 listeners; ProcessData delivers it on the thread
that calls ProcessData, as the SDK delivers callbacks.
