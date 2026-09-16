# RPCRT4 Wine test baseline

This directory contains ReactOS' Wine-derived RPCRT4 tests. Wine is an important
implementation reference and synchronization source, but it is not an authority
on Windows semantics. A ReactOS/Wine difference must not be removed solely
because current Wine differs.

## Pinned baseline

The initial comparison baseline is:

- ReactOS: `d09a3600606a65536416740a4eecf8a092d61c8b`
- Wine: `1a3eb798fa2479201823ee94b6a8e55d23907e9c`
- ReactOS tests: `modules/rostests/winetests/rpcrt4/`
- Wine tests: `dlls/rpcrt4/tests/`

The comparison revision must be updated deliberately. Do not describe an
unrecorded moving Wine `master` as the baseline.

## Evidence hierarchy

For semantic compatibility work, use evidence in this order:

1. Microsoft protocol specifications, public API contracts, and other public
   Windows documentation.
2. Reproducible native Windows conformance tests.
3. Current Wine behavior at a recorded commit.
4. ReactOS behavior and platform-specific integration requirements.
5. Other independent implementations, such as Samba, when the preceding
   evidence is insufficient.

Wine may contain defects, incomplete implementations, historical behavior, or
intentional differences caused by its Unix-hosted architecture. Agreement with
Wine is therefore not sufficient evidence that a behavior is Windows-compatible.

## Semantic classifications

Use these labels when reviewing an actual ReactOS/Wine behavioral difference:

| Classification | Meaning |
| --- | --- |
| `PLATFORM` | ReactOS requires different integration because of a genuine ReactOS platform constraint. |
| `WINDOWS_COMPATIBILITY` | Evidence shows Windows behaves differently from Wine and ReactOS intentionally follows Windows. |
| `STALE_WINE_DELTA` | ReactOS carries older Wine-derived behavior and current Wine has behavior verified to match Windows better. |
| `WINE_GAP` | Wine lacks or incompletely implements behavior required by Windows. |
| `UNVERIFIED_DELTA` | ReactOS and Wine differ, but Windows behavior has not yet been established. |

`UNVERIFIED_DELTA` is the default semantic classification when source comparison
finds a difference and no Windows evidence has yet resolved it.

## Mechanical comparison

Run:

```text
python modules/rostests/winetests/rpcrt4/compare_wine.py /path/to/wine
```

The script accepts either the Wine checkout root or `dlls/rpcrt4/tests`
directly. By default it compares against the ReactOS test directory containing
the script. A separate ReactOS checkout can be supplied with `--reactos`.

The output records the Git revision detected for both source trees and classifies
files mechanically as:

- `IDENTICAL`
- `DIFFERS`
- `REACTOS_ONLY`
- `MISSING_FROM_REACTOS`

These are inventory labels only. In particular, `DIFFERS` does not imply that
ReactOS should be synchronized to Wine. Such files require focused review and,
when semantics differ, one of the semantic classifications above.

Use `--fail-on-difference` only when source identity is intentionally required;
the normal inventory command exits successfully even when differences exist.

## Initial focused review set

The first semantic review should prioritize files that directly affect the
RPC/COM/WMI compatibility path:

- `ndr_marshall.c` -- NDR sizing and marshalling semantics.
- `cstub.c` -- generated client/stub behavior and NDR call paths.
- `server.c` -- RPC server lifecycle and transport behavior.
- `rpc.c` -- public RPC API behavior and ReactOS compatibility adaptations.

The corresponding implementation files in `dll/win32/rpcrt4/` must be reviewed
against native Windows behavior before removing ReactOS-specific branches.

## Review record

For each semantic difference, record at minimum:

- ReactOS commit;
- Wine commit;
- affected source and test files;
- Microsoft documentation or protocol reference, when available;
- native Windows version and architecture used for testing;
- native observed behavior;
- semantic classification;
- implementation decision.

A patch justified only by "Wine does this" is incomplete evidence.
