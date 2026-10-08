# Interfaces and parallel ownership

## Shared contract sources

| Contract | Implementation | Specification |
| --- | --- | --- |
| Runtime/session/capability | gui-esp32s3-simulator/backend/RuntimeContract.* and QemuController.* | docs/contracts/runtime.schema.json |
| Project/topology/geometry | backend/ProjectDocument.* | docs/contracts/project.schema.json |
| Host transport v1.0 | backend/PeripheralTransport.* | docs/contracts/bus.schema.json and bus-transport.md |
| QEMU dependency probe | qemu-extensions/prototypes/hostbus | docs/contracts/qemu-hostbus-prototype.md |
| Linear DC primitive | qemu-extensions/electrical/net-dc.[ch] | docs/contracts/electrical-dc.md |

These paths are relative to the repository; backend shorthand means
gui-esp32s3-simulator/backend. Read actual headers before changing an API.

## Runtime behavior

Runtime phases are Idle, Validating, Launching, Connecting, Initializing, Running,
Paused, WaitingForDebugger, WaitingForDevice, Stopping, Stopped and Error.
QemuController exposes runtimeStatus(), runtimeCapabilities() and status/capability
signals. RuntimeStatus includes phase/message/sessionId/resetEpoch.

Running means acknowledged QMP execution, not merely a process-start signal.
Startup probes actual machine/QOM capabilities and acknowledges cached settings
before cont. QMP requests have correlated IDs and a monotonic 10-second deadline;
query deduplication prevents indefinite accumulation. Unverified step/breakpoint
operations are disabled in both backend and UI.

Capability maturity is catalogued → implemented → native-tested →
hardware-compared → qualified. Availability/reason are separate fields. Keep
binary/profile-specific evidence; absence/unavailability is not zero/valid data.

## Authoritative project graph

ProjectDocument retains raw invalid drafts/extensions plus typed components,
terminals, nets, parameters and geometry. **nets[].endpoints is connectivity**.
Geometry is presentation only. Stable opaque IDs use an absolute ASCII full match,
length 1–64; generated terminal UUID IDs are about 45 characters, not old 92-character
concatenations. Names may be arbitrary Unicode.

Public editing APIs include add/remove/rename component/net/terminal,
connectTerminal/disconnectTerminal, setParameter/setSimulator/setComponentAttributes,
moveComponent and profile/firmware/runtime updates. Edits use QUndoStack.
save/saveAs use QSaveFile with direct-write fallback disabled.

Invalid drafts can load/save and be repaired, but cannot Apply. Known paths are
absolute internally and rebased on Save As; foreign Windows/UNC/WSL/URI identities
are preserved with diagnostics rather than guessed into local resources. Undo
after Save As must retain the same referents.

Legacy v2 migration is explicit. The compatibility exports
legacyRuntimeJson(), legacyRuntimeJsonForDestination(path,error) and
legacyRuntimeJsonWithAbsoluteResources(error) reject unsupported topology/typed
parameters/metadata. They are controller/address configuration, not an electrical
runtime. Do not expose mixed v3 objects with deprecated devices/buses aliases to
make old tests pass.

## Transport responsibility

Frames are uint32 big-endian length + UTF-8 JSON. Local limits include 1 MiB frames,
64 KiB decoded payload, 4 MiB queues and 64 in-flight requests. JSON is strict:
duplicate keys, invalid UTF-8, depth >64, unknown fields and noncanonical counter/ID
strings fail. Session/connection UUIDs and epoch/topology/sequence/virtual time
are exact request context; uint64 wire values are decimal strings.

Handshake is host-first hello/peer ACK with version, capability and capacity
intersection. Reset is queued before cancellation notifications; reentrant sends
cannot bypass reset ordering. Partial frame and host-service watchdogs are bounded.

**sendResponse(const BusEnvelope &originalRequest, ...)** preserves the full old
context across asynchronous callbacks. Never answer by request ordinal alone: an
old ordinal 1 can otherwise resolve a new connection's ordinal 1.

The codec owns format/bounds/correlation. Native bus controllers own legal phases,
address/direction/length, ACK/CS/clock/IRQ/DMA and endpoint reachability. Integration
owns runtime waiting state and coordinating QMP reset with transport epochs.
Host wall time is not guest peripheral timeout.

## Ownership table for incoming workers

| Role | Safe file scope | Shared files reserved to integrator |
| --- | --- | --- |
| UX/model | BoardWorkspace.*, ProjectDocument.*, CircuitWorkspaceTest.cpp, ThemeManagerTest.cpp, project schema and UX docs | MainWindow.*, main.cpp, CMakeLists.txt |
| Runtime | QemuController.*, RuntimeContract.*, QemuLaunchOptions.* and their tests | GUI actions and root build scripts unless coordinated |
| Transport | PeripheralTransport.*, PeripheralTransportTest.cpp, bus contract/schema | Controller/native engine integration |
| QEMU hostbus | Prototype source/tests/docs and reviewed patch suggestions | Actual cached checkout, shared core wiring and patch-application tooling |
| Electrical | qemu-extensions/electrical and analytic tests | Native GPIO/RTC/ADC and graph adapter integration |
| Radio/SIMD reference | Fixture sources, build/acquisition tooling and host tests | Physical operation and recovery runner ownership |
| Integrator | Shared CMake/MainWindow, official/extension build tools, validation ledger | Coordinates all cross-role edits |

Original workers are named ux_plan, peripherals_plan and radio_simd_plan. Check
whether they are still running before assigning overlapping scope. The latest
documentation request asked them to finish current checks and freeze.

One owner runs the shared GUI/QEMU builds. Workers use isolated builds and provide
tracked patches against the pinned core; no hidden sole-copy edits in ignored
caches. If a new owner changes a shared contract, publish the header/schema change,
migration implications and failing/passing consumers before other workers proceed.

## Licensing and distribution boundary

Keep the QEMU core as a separate executable. New QEMU/DC source carries
GPL-2.0-or-later; patches preserve upstream file licenses, including LGPL sources.
The test-only Qt peer is MIT and links the host transport, not QEMU. Do not assume
closed UI means distributed modified QEMU may remain undisclosed. No project-wide
proprietary license, publication, push or deployment was performed.
