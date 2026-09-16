# ReactOS RPC, COM, and WMI compatibility roadmap

## Status

This document is a working engineering roadmap, not a specification and not an
authoritative description of Windows internals. Priorities and dependencies must
be revised when source review, native Windows conformance tests, or public
protocol documentation contradict an assumption made here.

The source baseline for the initial review is ReactOS
`d09a3600606a65536416740a4eecf8a092d61c8b`. Wine comparisons use explicitly
pinned revisions recorded in `modules/rostests/winetests/rpcrt4/BASELINE.md`.

## Scope

This roadmap covers ReactOS RPC, COM, DCOM, and WMI compatibility. DDE is out of
scope for this work. That does not imply that DDE is unimportant to ReactOS as a
legacy Windows-compatibility feature; it is simply independent of this dependency
graph.

The compatibility target is Windows behavior. Wine is an important implementation
reference and synchronization source, but it is not an authority on Windows
semantics. Wine may contain defects, incomplete behavior, historical behavior, or
deliberate Unix-hosted implementation differences.

The immediate objective is useful functional compatibility. Complete security
fidelity is not a prerequisite for local RPC, local COM, or local WMI milestones.
However, security that is required to establish a particular transport or
protocol interaction is part of that milestone. Remote named-pipe RPC depends on
the SMB session beneath it, and supported Windows DCOM systems enforce security
requirements for remote activation.

## Evidence hierarchy

For every semantic difference, use evidence in this order:

1. Microsoft protocol specifications, public API contracts, and other public
   Windows documentation.
2. Reproducible native Windows conformance tests when documentation is incomplete
   or does not define the observed runtime behavior.
3. Current Wine behavior at an explicitly recorded commit.
4. ReactOS behavior and platform-specific integration requirements.
5. Other independent implementations, such as Samba, when earlier evidence is
   insufficient.

A ReactOS/Wine source difference is not itself evidence that ReactOS is wrong.
Use the semantic classifications documented in
`modules/rostests/winetests/rpcrt4/BASELINE.md` before removing an intentional
ReactOS difference.

The classifications are:

| Classification | Meaning |
| --- | --- |
| `PLATFORM` | ReactOS requires different integration because of a genuine ReactOS platform constraint. |
| `WINDOWS_COMPATIBILITY` | Public Windows evidence shows that the ReactOS behavior is required even though Wine differs. |
| `STALE_WINE_DELTA` | ReactOS retains older Wine-derived behavior and current Wine is verified to match Windows better. |
| `WINE_GAP` | Wine lacks or incompletely implements required Windows behavior. |
| `UNVERIFIED_DELTA` | ReactOS and Wine differ, but Windows behavior has not yet been established. |

`UNVERIFIED_DELTA` is the default classification for an unexplained semantic
source difference.

## Dependency model

```text
WIDL-generated NDR metadata
             |
             v
          RPCRT4 <------------------ RPCSS endpoint mapper
             |
             v
       COMBASE / OLE32 <------------ RPCSS class-object registry
          |       |
          |       +----------------> LocalServer32
          |       |
          |       +----------------> DLLHOST surrogate
          |
          +------------------------> WBEMPROX
                                      |
                                      v
                                   WBEMDISP

Remote COM additionally requires:
    remote RPC transports
    remote endpoint discovery
    activation SCM interfaces
    OXID resolution
    ORPC
    IRemUnknown lifetime
    resolver bindings
    authentication/security
```

This is a dependency graph, not a strict serial schedule. Independent tests should
run as soon as their prerequisites exist.

In particular, Microsoft documents `IWbemLocator` as an in-process COM server,
and ReactOS WBEMPROX exposes `CLSID_WbemLocator` through `DllGetClassObject`.
Therefore a local WBEMPROX smoke baseline can be established independently of
DLLHOST and remote DCOM. More complex local COM and RPC work may still be needed
for other components and for architectural fidelity.

## Baseline source observations

These observations are tied to the source baseline above and must be rechecked
when the underlying code changes.

- `dll/win32/rpcrt4/rpc_transport.c` implements `ncalrpc` using named pipes under
  `\\.\pipe\lrpc\` rather than LPC/ALPC.
- The same file contains ReactOS-specific named-pipe security-descriptor creation.
  It currently grants Everyone and Anonymous generic read/write access and grants
  Administrators full access. This is existing security behavior that must be
  catalogued now even though Windows ACL fidelity is a later milestone.
- RPCSS already hosts local/named-pipe endpoint-mapper functionality. TCP port 135
  endpoint mapping must be treated as a separate remote interoperability target.
- the current `dllhost.exe` implementation is not yet a functional COM surrogate.
- the current DcomLaunch service does not by itself constitute a Windows-compatible
  Remote Activation implementation.
- WBEMPROX already exposes a substantial in-process WMI COM implementation.
- `ndr_stubless.c` contains a ReactOS guard preventing a generic unbind callback
  when the bind callback returned `NULL`; Microsoft MIDL documentation requires
  this behavior.
- `ndr_marshall.c` contains a ReactOS-specific union pointer-sizing difference
  whose Windows behavior has not yet been established.

## Priority 0 -- Evidence, regression baseline, and smoke baselines

### P0.1 RPCRT4 Wine test baseline

Compare `modules/rostests/winetests/rpcrt4/` against Wine's
`dlls/rpcrt4/tests/` at explicitly pinned commits. Do not replace the ReactOS
directory wholesale.

The baseline must record:

- ReactOS commit;
- Wine commit;
- mechanical source differences;
- semantic classification for reviewed differences;
- Windows versions and architectures used for conformance tests.

`compare_wine.py` is an inventory tool only. `DIFFERS` must never imply
"synchronize to Wine".

Exit gate: every current RPCRT4 test difference is reproducibly enumerable and
can be escalated to a Windows conformance test.

### P0.2 Native Windows conformance harness

Maintain repeatable native-Windows runs for behavior that documentation does not
fully specify. Record Windows version, build, architecture, inputs, and observed
outputs.

A useful minimum matrix is one NT 6.x system where practical plus a supported
modern Windows release for current interoperability. Architecture-sensitive NDR
behavior should be exercised on both x86 and x64 where possible.

Exit gate: semantic decisions can be reproduced independently of Wine.

### P0.3 Local WMI smoke baseline

Do not wait for the complete RPC/COM roadmap before determining what the existing
WBEMPROX implementation can already do.

Run a minimal in-process WMI client that performs:

```text
CoInitializeEx
    |
    v
CoCreateInstance(CLSID_WbemLocator, CLSCTX_INPROC_SERVER)
    |
    v
IWbemLocator::ConnectServer(local namespace)
    |
    v
IWbemServices
```

Record class lookup, one deterministic instance enumeration, and one simple WQL
query. The purpose is diagnostic: identify the current WMI baseline and separate
existing WBEMPROX defects from later RPC/COM defects.

Exit gate: current local WBEMPROX capability is documented with reproducible test
results. This does not imply that local WMI is complete.

## Priority 1 -- Known and high-value NDR semantic differences

### P1.1 Encapsulated-union buffer sizing

Test scalar, null-pointer, and non-null pointer arms with both zero and non-zero
initial `BufferLength`. Use a valid WIDL/MIDL type-format encoding. Establish
native Windows behavior before changing the current ReactOS-specific branch.

The initial probe is
`modules/rostests/winetests/rpcrt4/ndr_union.c`. It intentionally accepts the two
currently known candidate outcomes until native Windows establishes the target.

Exit gate: the probe is tightened to the Windows-observed `BufferLength` and
`PointerLength` behavior. Remove the ReactOS-specific implementation branch only
if the Windows result permits a common implementation; otherwise retain and
classify the Windows-compatible divergence.

### P1.2 Generic binding `NULL` semantics

Retain the ReactOS behavior that avoids calling the generic unbind routine when
the user bind routine returns `NULL`.

Microsoft's MIDL `[handle]` documentation states that if the user bind routine
returns `NULL`, the corresponding unbind routine is not called. Current Wine and
ReactOS differ here, so this is presently a documented Windows-compatibility
case rather than evidence that ReactOS is stale.

Add a generated-IDL conformance test that records bind and unbind call counts and
runs through the actual `FC_BIND_GENERIC` client path.

Current semantic classification: `WINDOWS_COMPATIBILITY`.

Exit gate: the documented behavior is covered by a regression test. If Wine still
differs, consider an upstream Wine fix rather than removing the ReactOS guard.

### P1.3 Full-pointer functional growth

Test translation-table growth and preservation of existing RefId mappings before
changing allocation-failure behavior.

A normal successful-growth test is useful coverage, but it does not prove
allocation-failure atomicity. Any OOM hardening must define whether the entire
operation, rather than only the two array replacements, remains consistent after
failure.

Exit gate: successful growth is functionally correct and repeatable; OOM semantics
are separately defined before implementation changes are described as atomic.

## Priority 2 -- RPCRT4 functionality required by COM and WMI

### P2.1 NDR dependency matrix

Audit and implement only the NDR constructs required by the targeted COM/WMI
interfaces before pursuing broad RPCRT4 completeness. Cover interface pointers,
structures, unions, arrays, strings, context handles, generated stubs, and
full-pointer behavior where exercised.

Full-pointer out-of-memory work must distinguish table-allocation atomicity from
whole-operation atomicity. Do not claim failure-atomic insertion while hash-side
state or `NextRefId` can already have changed.

### P2.2 Required RPCRT4 API surface

Compare the ReactOS RPCRT4 exports, current Wine exports, and public Microsoft RPC
APIs, but gate implementation by the interfaces required by the active COM/WMI
vertical slice.

Classify required APIs as implemented, partial, stub, absent, or security-deferred.
Do not require complete Windows RPCRT4 export parity before proceeding.

### P2.3 Dependency-driven core audit

Review `rpc_binding.c`, `rpc_assoc.c`, `rpc_message.c`, `rpc_server.c`, and
`rpcrt4_main.c` only where tests or the COM/WMI dependency matrix expose a gap.
Do not perform a broad synchronization solely to reduce textual divergence.

Exit gate: all NDR and non-security RPCRT4 behavior exercised by the local COM/WMI
vertical slice is available and testable.

## Priority 3 -- Reliable local RPC

### P3.1 `ncalrpc`

ReactOS currently implements `ncalrpc` using local named pipes. That is acceptable
for the functional milestone if externally visible RPC behavior is compatible.

Test endpoint creation, concurrent calls, shutdown, reconnect, endpoint reuse,
cancellation, client termination, and server termination.

Security and caller-identity fidelity are not completion criteria here, but any
existing security-related behavior encountered during testing must be recorded.

### P3.2 local `ncacn_np`

Run the same lifecycle matrix over local named-pipe RPC.

### P3.3 local endpoint mapper

Validate registration, lookup, dynamic endpoint resolution, unregister, server
termination, stale-registration cleanup, and RPCSS restart over the existing local
EPM transports.

Exit gate: a local client can discover and invoke a dynamically registered local
RPC server without knowing its endpoint in advance.

## Priority 4 -- Local COM vertical slice

Audit COMBASE/OLE32/RPCSS only for functionality needed by a working local
out-of-process COM server:

- apartment initialization;
- class registration and lookup;
- `LocalServer32` launch;
- OBJREF/interface-pointer marshalling;
- proxy/stub invocation;
- `IUnknown` identity;
- lifetime and process teardown.

Current local activation architecture should be tested as implemented before
redesigning it around remote DCOM assumptions.

Exit gate: Process A activates an object in Process B, obtains a normally
marshalled interface, invokes it, preserves identity, and releases it correctly.

## Priority 5 -- Expand local WMI coverage

Use the P0.3 smoke baseline as the starting point rather than assuming that local
WMI is blocked by every preceding RPC/COM milestone.

Test deterministic operations in increasing complexity:

- namespace connection;
- class retrieval;
- instance enumeration;
- WQL queries;
- property reads;
- supported methods;
- broader class enumeration;
- asynchronous operations where implemented.

Distinguish:

- COM activation failure;
- WMI object-model failure;
- query-parser failure;
- provider/data failure;
- security facade behavior.

Then validate WBEMDISP (`SWbemLocator`, `SWbemServices`, `SWbemObjectSet`,
property access, enumeration, and scripting clients).

Exit gate: conventional local WMI COM clients perform deterministic useful work.

## Priority 6 -- DLLHOST surrogate activation

The current DLLHOST implementation must first be treated as an implementation gap,
not as an already working surrogate host.

Before changing activation code, validate Windows registration and launch behavior
for the relationship among CLSID, AppID, `InprocServer32`, and `DllSurrogate`.
Microsoft documents `DllSurrogate` under the AppID key, and multiple CLSIDs may
share one AppID and surrogate.

Implement and test:

- AppID lookup from the CLSID registration;
- `DllSurrogate` selection;
- `InprocServer32` loading;
- COM initialization;
- class-factory registration;
- method invocation;
- DLL unload rules;
- process lifetime and shutdown;
- malformed registration and launch failures.

Do not assume that a `/Processid:{GUID}` value is necessarily the CLSID; establish
the Windows/AppID semantics with documentation and a native conformance test before
changing the launcher/host contract.

Exit gate: an in-process COM DLL registered for surrogate execution is activated
out of process and invoked through a normal proxy.

## Priority 7 -- Remote RPC interoperability

### P7.1 remote `ncacn_np`

First prove the SMB redirector/server, named-pipe transport, session setup, and
credential path independently. `ncacn_np` is RPC directly over SMB, so an RPC
binding with no RPCRT4 authentication service is not necessarily an unauthenticated
network connection.

Then test Windows-to-ReactOS, ReactOS-to-Windows, and ReactOS-to-ReactOS RPC over
named pipes, recording how the underlying SMB session was established.

### P7.2 static `ncacn_ip_tcp`

Test explicit TCP ports before dynamic endpoint mapping, including Windows and
ReactOS in both client/server directions. Cover IPv4 and hostname resolution first;
IPv6 tower fidelity can remain a later compatibility item if it is not required by
the active interoperability target.

### P7.3 TCP endpoint mapper

Treat TCP EPM as separate from local EPM. Implement and test RPCSS ownership of
TCP port 135, TCP tower creation/decoding, dynamic endpoint registration, remote
lookup, and subsequent connection to the returned dynamic endpoint.

Exit gate: a client can resolve a dynamically registered TCP endpoint through
port 135 and invoke the interface.

## Priority 8 -- Remote DCOM

Do not infer DCOM completeness from the presence of a DcomLaunch service. Define
and implement the missing protocol pieces explicitly.

Treat the following as separate work packages:

- Remote Activation;
- OXID resolver and object-exporter lifecycle;
- ORPC metadata;
- `IRemUnknown` QueryInterface/AddRef/Release behavior;
- resolver bindings;
- remote activation authentication and required DCOM security.

Supported Windows systems enforce DCOM activation hardening. Non-anonymous remote
activation must therefore interoperate at the authentication level required by the
Windows peer; required activation security is part of this milestone, not Priority
10 cleanup.

Exit gate: a Windows/ReactOS pair remotely activates a simple COM server, resolves
its object exporter, invokes an interface, manages references, and tears the object
down correctly.

## Priority 9 -- Remote WMI

Remote WMI depends on remote RPC, DCOM activation, OXID resolution,
`IRemUnknown`, required DCOM authentication, and WMI namespace security.

Test remote `IWbemLocator::ConnectServer`, `IWbemServices`, queries, object
retrieval, enumeration, and supported method execution. Separate WMI
authorization failures from DCOM activation failures.

Exit gate: explicitly supported remote WMI operations work across the
Windows/ReactOS boundary using a documented security configuration.

## Priority 10 -- Broader security fidelity

After the security required by earlier protocol milestones works, improve Windows
fidelity for:

- the existing ReactOS-specific RPCRT4 named-pipe DACL;
- endpoint security descriptors;
- SSPI integration;
- NTLM and Kerberos;
- RPC security callbacks;
- authorization-context APIs;
- impersonation and delegation;
- COM launch and access permissions;
- WMI namespace security;
- token and access-check behavior.

Security required by an earlier milestone remains part of that earlier milestone.
Do not add ad-hoc RPC or COM workarounds for behavior that properly belongs in the
ReactOS security subsystem.

## Priority 11 -- Residual compatibility

Defer unless a concrete dependency is found:

- MIDL pipes;
- native LPC/ALPC-backed `ncalrpc`;
- complete IPv6 endpoint-tower interoperability;
- complete RPCRT4 export parity;
- broad Wine synchronization not required by the active compatibility path.

## Execution model

The main RPC/COM engineering path is:

```text
Evidence baseline
      |
      v
Known NDR semantics
      |
      v
COM/WMI-required RPCRT4 behavior
      |
      v
Local RPC + local EPM
      |
      v
Local out-of-process COM
```

Local WMI has an early diagnostic path and a later completeness path:

```text
Evidence baseline --> WBEMPROX smoke baseline
                         |
                         +--> expand WMI coverage as independent defects are found
                         |
Local COM ---------------+--> exercise any WMI behavior that genuinely depends on it
```

Remote work proceeds as a separate branch after the relevant transport foundations
are stable:

```text
remote ncacn_np --------\
                         +--> remote endpoint discovery --> remote DCOM --> remote WMI
static ncacn_ip_tcp ----/
```

DLLHOST is a parallel local-COM completeness track after basic local COM behavior
is understood. It is not a prerequisite for the first WBEMPROX smoke baseline.

## Completion gates

### RPCRT4 local readiness

ReactOS can reliably create, register, discover, connect to, invoke, shut down, and
reconnect local RPC interfaces using the NDR constructs required by the active COM
workload.

### Local COM

Process A can activate a COM local-server object in Process B, obtain a normally
marshalled interface pointer, invoke methods, preserve `IUnknown` identity, and
manage object lifetime correctly.

### Local WMI

A normal COM client can instantiate `IWbemLocator`, connect to a local ReactOS WMI
namespace, obtain `IWbemServices`, and execute deterministic class, instance, and
WQL operations.

### Remote RPC

Static and dynamically resolved remote named-pipe/TCP RPC interoperates with the
claimed Windows peer configurations. TCP dynamic discovery includes endpoint
mapping through port 135.

### DLLHOST

A DLL COM server configured for surrogate activation can be activated and invoked
through a functional ReactOS surrogate host.

### Remote DCOM

Remote activation, OXID resolution, ORPC, remote lifetime, and required activation
security work across the Windows/ReactOS boundary.

### Remote WMI

Remote WMI works over the completed DCOM path and obeys the explicitly supported
namespace-security model.

## Review rule

A semantic patch justified only by "Wine does this" is incomplete. Every such
patch should answer:

```text
What does Microsoft document?
What does native Windows do when documentation is insufficient?
What does the pinned Wine revision do?
What does the pinned ReactOS revision do?
Why does ReactOS need to change?
How is the difference classified?
What test prevents regression?
```

Only then should an intentional ReactOS/Wine semantic difference be removed.

## Primary public references

- Microsoft MIDL `[handle]` attribute:
  https://learn.microsoft.com/en-us/windows/win32/midl/handle
- Microsoft MS-RPCE, SMB (`ncacn_np`):
  https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rpce/7063c7bd-b48b-42e7-9154-3c2ec4113c0d
- Microsoft `IWbemLocator`:
  https://learn.microsoft.com/en-us/windows/win32/api/wbemcli/nn-wbemcli-iwbemlocator
- Microsoft `IWbemLocator::ConnectServer`:
  https://learn.microsoft.com/en-us/windows/win32/api/wbemcli/nf-wbemcli-iwbemlocator-connectserver
- Microsoft `DllSurrogate`:
  https://learn.microsoft.com/en-us/windows/win32/com/dllsurrogate
- Microsoft surrogate activation registration:
  https://learn.microsoft.com/en-us/windows/win32/com/registering-the-dll-server-for-surrogate-activation
- Microsoft KB5004442, DCOM activation hardening:
  https://support.microsoft.com/help/5004442
