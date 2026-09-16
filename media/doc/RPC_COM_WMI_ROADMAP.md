# ReactOS RPC, COM, and WMI compatibility roadmap

## Scope

This roadmap covers ReactOS RPC, COM, DCOM, and WMI compatibility. DDE is out of
scope for this work. That does not imply that DDE is unimportant to ReactOS as a
legacy Windows-compatibility feature; it is simply independent of this dependency
graph.

The compatibility target is Windows behavior. Wine is an important implementation
reference and synchronization source, but it is not an authority on Windows
semantics. Wine may contain defects, incomplete behavior, historical behavior, or
deliberate Unix-hosted implementation differences.

For every semantic difference, use evidence in this order:

1. Microsoft protocol specifications, public API contracts, and other public
   Windows documentation.
2. Reproducible native Windows conformance tests.
3. Current Wine behavior at an explicitly recorded commit.
4. ReactOS behavior and platform-specific integration requirements.
5. Other independent implementations, such as Samba, when earlier evidence is
   insufficient.

A ReactOS/Wine source difference is not itself evidence that ReactOS is wrong.
Use the semantic classifications documented in
`modules/rostests/winetests/rpcrt4/BASELINE.md` before removing an intentional
ReactOS difference.

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
    endpoint mapping
    activation SCM interfaces
    OXID resolution
    ORPC
    IRemUnknown lifetime
    resolver bindings
    authentication/security
```

Local WMI is intentionally ahead of DLLHOST and remote DCOM. WBEMPROX is an
in-process COM server, so useful local WMI functionality does not require the
remote DCOM stack.

## Priority 0 -- Evidence and regression baseline

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

Exit gate: semantic decisions can be reproduced independently of Wine.

## Priority 1 -- Known NDR semantic differences

### P1.1 Encapsulated-union buffer sizing

Test scalar, null-pointer, and non-null pointer arms with both zero and non-zero
initial `BufferLength`. Use valid WIDL/MIDL type-format encodings. Establish
native Windows behavior before changing the current ReactOS-specific branch.

Exit gate: ReactOS implements the Windows-observed `BufferLength` and
`PointerLength` behavior.

### P1.2 Generic binding NULL semantics

Retain the ReactOS behavior that avoids calling the generic unbind routine when
the user bind routine returns `NULL` unless native Windows evidence disproves it.
Microsoft MIDL documentation states that an unbind routine is not called when the
bind routine returns `NULL`.

Add a conformance test that records bind and unbind call counts.

Exit gate: the behavior is covered by a regression test and classified as
`WINDOWS_COMPATIBILITY`, `STALE_WINE_DELTA`, or another evidence-backed category.

### P1.3 Full-pointer functional growth

Test translation-table growth and preservation of existing RefId mappings before
changing allocation-failure behavior.

Exit gate: successful growth is functionally correct and repeatable.

## Priority 2 -- NDR required by COM and WMI

Audit and implement only the NDR constructs required by the targeted COM/WMI
interfaces before pursuing broad RPCRT4 completeness. Cover interface pointers,
structures, unions, arrays, strings, context handles, generated stubs, and
full-pointer behavior where exercised.

Full-pointer out-of-memory work must distinguish table-allocation atomicity from
whole-operation atomicity. Do not claim failure-atomic insertion while hash-side
state or `NextRefId` can already have changed.

Exit gate: all NDR constructs exercised by the local COM/WMI vertical slice pass.

## Priority 3 -- Reliable local RPC

### P3.1 `ncalrpc`

ReactOS currently implements `ncalrpc` using local named pipes. That is acceptable
for the functional milestone if externally visible RPC behavior is compatible.

Test endpoint creation, concurrent calls, shutdown, reconnect, endpoint reuse,
cancellation, client termination, and server termination.

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

Current local activation architecture should be tested as implemented rather than
prematurely redesigned around remote DCOM.

Exit gate: Process A activates an object in Process B, obtains a normally
marshalled interface, invokes it, preserves identity, and releases it correctly.

## Priority 5 -- Local WMI

Start with WBEMPROX:

```text
CoInitializeEx
    |
    v
CoCreateInstance(CLSID_WbemLocator)
    |
    v
IWbemLocator::ConnectServer()
    |
    v
IWbemServices
```

Test deterministic operations in increasing complexity: class retrieval,
instance enumeration, WQL queries, property reads, supported methods, and broader
class enumeration. Distinguish COM activation, WMI object-model, query parser,
and provider/data failures.

Then validate WBEMDISP (`SWbemLocator`, `SWbemServices`, `SWbemObjectSet`,
property access, enumeration, and scripting clients).

Exit gate: conventional local WMI COM clients perform deterministic useful work.

## Priority 6 -- DLLHOST surrogate activation

Implement actual surrogate hosting, including `/Processid:{AppID}` handling,
AppID resolution, `DllSurrogate`, `InprocServer32`, COM initialization,
class-factory registration, lifetime, unload, and shutdown behavior.

Exit gate: an in-process COM DLL registered for surrogate execution is activated
out of process and invoked through a normal proxy.

## Priority 7 -- Remote RPC interoperability

### P7.1 remote `ncacn_np`

First prove the SMB redirector/server, named-pipe transport, session setup, and
credential path independently. Then test Windows-to-ReactOS,
ReactOS-to-Windows, and ReactOS-to-ReactOS RPC over named pipes.

### P7.2 static `ncacn_ip_tcp`

Test explicit TCP ports before dynamic endpoint mapping, including Windows and
ReactOS in both client/server directions.

### P7.3 TCP endpoint mapper

Treat TCP EPM as separate from local EPM. Implement and test RPCSS ownership of
TCP port 135, TCP tower creation/decoding, dynamic endpoint registration, remote
lookup, and subsequent connection to the returned dynamic endpoint.

Exit gate: a client can resolve a dynamically registered TCP endpoint through
port 135 and invoke the interface.

## Priority 8 -- Remote DCOM

Implement as separate work packages:

- Remote Activation;
- OXID resolver and object exporter lifecycle;
- ORPC metadata;
- `IRemUnknown` QueryInterface/AddRef/Release behavior;
- resolver bindings;
- remote activation authentication and required DCOM security.

Remote DCOM must not be designed around an intentionally insecure activation
path that cannot interoperate with supported Windows systems.

Exit gate: a Windows/ReactOS pair remotely activates a simple COM server, invokes
an interface, manages references, and tears the object down correctly.

## Priority 9 -- Remote WMI

Remote WMI depends on remote RPC, DCOM activation, OXID resolution,
`IRemUnknown`, required DCOM authentication, and WMI namespace security.

Test remote `IWbemLocator::ConnectServer`, `IWbemServices`, queries, object
retrieval, enumeration, and supported method execution. Separate WMI
authorization failures from DCOM failures.

Exit gate: explicitly supported remote WMI operations work across the
Windows/ReactOS boundary.

## Priority 10 -- Broader security fidelity

After the protocol-required security path works, improve fidelity for named-pipe
ACLs, endpoint security descriptors, SSPI, NTLM, Kerberos, RPC security callbacks,
impersonation/delegation, COM launch/access permissions, WMI namespace security,
and token/access-check behavior.

Security required by an earlier milestone remains part of that earlier milestone.

## Priority 11 -- Residual compatibility

Defer unless a concrete dependency is found:

- MIDL pipes;
- native LPC/ALPC-backed `ncalrpc`;
- IPv6 endpoint towers;
- complete RPCRT4 export parity;
- broad Wine synchronization not required by the active compatibility path.

## Critical path

```text
Evidence baseline
      |
      v
Known NDR semantics
      |
      v
COM/WMI-required NDR
      |
      v
Local RPC + local EPM
      |
      v
Local COM
      |
      v
Local WMI
```

Remote work proceeds as a parallel branch after the local foundations are stable:

```text
remote ncacn_np --------\
                         +--> remote endpoint discovery --> remote DCOM --> remote WMI
static ncacn_ip_tcp ----/
```

DLLHOST is a parallel local-COM completeness track after the first working local
COM milestone.

## Review rule

A semantic patch justified only by "Wine does this" is incomplete. Every such
patch should answer:

```text
What does Microsoft document?
What does native Windows do?
What does current Wine do?
What does current ReactOS do?
Why does ReactOS need to change?
How is the difference classified?
```

Only then should an intentional ReactOS/Wine semantic difference be removed.
