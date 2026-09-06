# pyCircuit 6 Decisions

<!-- markdownlint-disable MD032 MD036 -->

This file is the semantic decision corpus for pyCircuit 6. Decisions 0001–0147
originated during the pyc4.0 architecture work; their original context and
source wording remain intact as historical rationale. Later decisions may
supersede earlier ones explicitly. When two decisions conflict, the later
accepted decision governs.

The product-facing language and API contract are defined by
`docs/v6_PyCircuit_Specification.md`. The implementation plan is
`docs/pyc6-plan.md`.

## Decision 0001: C++ sim object model and module boundary

**Status:** Accepted

**Context / Goal**
pyc4.0 targets ultra-large designs with scalable C++ functional simulation.
We need a first-class simulation object model that maps to module instances,
with strong DFX/probe support and explicit state ownership.

**Decision**
- **SimObject is 1:1 with a pyCircuit `@module` instance** (option 1).
- Each SimObject must provide:
  - `tick()`
  - `transfer()`
- Each SimObject owns internal state generated from:
  - `reg`-like state
  - `mem`-like state
- **Combinational logic inside a module may be flattened** (no requirement to preserve sub-expression structure), but module-instance boundaries are preserved as SimObjects.
- Frontend/IR contract: **pyCircuit MLIR must be able to emit module SimObjects** (i.e., the backend has the information required to generate C++ objects per module instance).

**Implications**
- Backend C++ emission must generate a class/struct per module type plus an instantiation graph that creates per-instance objects.
- DFX/probe pathing should naturally align to module-instance hierarchy.
- `tick/transfer` split implies a 2-phase cycle model (update vs commit), enabling deterministic scheduling and scalable tracing.

**Source**
- User direction in #linx-core (2026-03-01): "选项1 ... tick和transfer ... state由reg和mem产生 ... comb flatten ... mlir emit module sim obj".

## Decision 0002: DFX/probe naming paths are hierarchical but must stay short

**Status:** Accepted

**Context / Goal**
Ultra-large designs need stable, human-readable hierarchical paths for DFX,
trace, breakpointing, and interactive inspection. At the same time, deeply
nested hierarchies can produce overly long strings that harm usability and
runtime overhead.

**Decision**
- Use **hierarchical naming paths** for SimObjects and probes (option A).
- The naming scheme must be **simple** and explicitly designed to **avoid overly long hierarchy strings**.

**Notes / Open follow-ups**
- We still need to decide the concrete shortening mechanism (examples):
  - allow per-module `short_name` / alias
  - path segment hashing beyond a depth threshold
  - optional elision of intermediate hierarchy with stable anchors

**Source**
- User direction in #linx-core (2026-03-01): "a 并且命名规则要简单，避免层级字符串过长".

## Decision 0003: Probe is a unified concept with typed inference (wire/reg/mem/statevar)

**Status:** Accepted

**Context / Goal**
We want strong DFX for ultra-large designs and explicitly need non-wire probes
(e.g. registers, memories, and structured state). At the API surface, we want a
single concept called a "probe" rather than forcing users to manually choose
probe subclasses.

**Decision**
- Use a unified concept: **everything is a `probe`** at the user-facing API.
- Internally, probes have inferred kinds/types, at minimum:
  - wire-like
  - reg-like
  - mem-like
  - statevar-like
- The system performs **type/kind inference** to decide probe behavior and tooling.

**Implications**
- Frontend must preserve enough metadata for probe-kind inference.
- C++ runtime must expose a uniform probe handle/descriptor while still allowing
  kind-specific operations (e.g. mem watch/dump).

**Source**
- User direction in #linx-core (2026-03-01): "a 但是都需要统称为probe，我们有类型推导".

## Decision 0004: Central ProbeRegistry for scalable lookup

**Status:** Accepted

**Context / Goal**
Ultra-large designs require probe lookup to be efficient and tooling-friendly.
A naive tree-walk on every lookup does not scale.

**Decision**
- Use a **centralized `ProbeRegistry`** (option A).
- Registry must support efficient lookup by:
  - hierarchical path (exact match)
  - wildcard/glob match
  - inferred probe kind/type (wire/reg/mem/statevar)

**Implications**
- SimObjects must register their probes deterministically during construction/elaboration.
- Registry must be able to operate with short-path naming rules (Decision 0002).

**Source**
- User direction in #linx-core (2026-03-01): "a" for centralized registry.

## Decision 0005: Two-phase probe sampling (pre/post transfer)

**Status:** Accepted

**Context / Goal**
With a `tick()` / `transfer()` simulation split, debugging needs visibility into
both the computed next-state and the committed state.

**Decision**
- Support two sampling points (option A):
  - **pre-transfer** sampling
  - **post-transfer** sampling

**Implications**
- Trace/DFX tooling can choose pre, post, or both.
- Probe evaluation must be well-defined at each phase.

**Source**
- User direction in #linx-core (2026-03-01): "a" for dual-phase sampling.

## Decision 0006: Memory observability supports hash/watch/dump modes

**Status:** Accepted

**Context / Goal**
Memory state is large; DFX must scale without dumping full contents every cycle.
We need modes that support fast regression checking, targeted debugging, and
on-demand snapshots.

**Decision**
- MemProbe observability supports three modes (option A):
  1) **hash**: `mem_hash()` per-cycle or periodic hashing
  2) **watch**: `mem_watch(range)` event stream for reads/writes in a range
  3) **dump**: `mem_dump(trigger)` snapshot on trigger

**Implications**
- Runtime needs a stable hashing scheme and event encoding.
- Trigger and range filter semantics must be standardized.

**Source**
- User direction in #linx-core (2026-03-01): "a" for mem hash/watch/dump.

## Decision 0007: spec supports runtime reflection

**Status:** Accepted

**Context / Goal**
pyc4.0 needs scalable DFX, probe inference, and large-module integration.
That requires being able to introspect structured types at runtime (field list,
widths, nesting, and paths) rather than losing all type info after elaboration.

**Decision**
- `spec` must support **runtime reflection** (option A).

**Implications**
- The frontend must preserve spec metadata into the emitted artifacts.
- The C++ runtime/templates must be able to represent/specify reflective schemas.

**Source**
- User direction in #linx-core (2026-03-01): "a" for spec runtime reflection.

## Decision 0008: spec uses a layered type system (Bits/Array/Struct/Union/Signature) with parameterization

**Status:** Accepted

**Context / Goal**
Ultra-large module interfaces need expressive, structured types that can be used
consistently across Python frontend authoring, MLIR emission, and C++ simulation
code generation. DFX and probe inference also benefit from explicit type kinds.

**Decision**
- `spec` will provide a layered type system (option A):
  - `Bits(width, signed)`
  - `Array(n, elem)`
  - `Struct(fields...)`
  - `Union(variants...)`
  - `Signature(directed ports...)`
- The type system must support **parameterization** (e.g. via valueclass/config objects).

**Implications**
- Frontend API needs stable constructors/builders for these types.
- MLIR and emitted artifacts must preserve these type kinds.
- C++ templates should map these type kinds into generated storage/ports.

**Source**
- User direction in #linx-core (2026-03-01): "a" for layered spec type system.

## Decision 0009: spec fields have canonical path strings

**Status:** Accepted

**Context / Goal**
We need stable, tool-friendly naming for DFX, probe registration, trace, and
cross-language mapping (Python ↔ MLIR ↔ C++). Structured types must still have a
canonical string path form.

**Decision**
- Each spec field must have a **canonical path string** representation (option A),
  e.g. `foo.bar[3].baz`.

**Implications**
- Reflection APIs must expose canonical paths.
- Path strings should remain simple/short in line with Decision 0002.

**Source**
- User direction in #linx-core (2026-03-01): "a" for canonical spec field paths.

## Decision 0010: Remove global cycle-aware; allow optional module-local cycle-aware sub-DSL

**Status:** Superseded by Decision 0148

**Context / Goal**
pyc4.0 focuses on scalable C++ functional simulation with explicit state and
module SimObjects. The legacy global cycle-aware signal model adds complexity
and conflicts with the desired object/state semantics.

**Decision**
- **No global cycle-aware signal system** in pyc4.0.
- Default timing/state semantics are expressed via explicit `reg`/`mem` state and
  `tick()`/`transfer()`.
- Optionally, allow a **module-local cycle-aware sub-DSL** contained within a
  single module boundary (clear isolation).

**Implications**
- Frontend APIs and docs should no longer center cycle-aware programming.
- If the module-local sub-DSL exists, it must compile down to ordinary reg/mem +
  comb logic within the module, without leaking cycle annotations across module
  boundaries.

**Source**
- User direction in #linx-core (2026-03-01): "a" for global removal + module-local option.

**Supersession note**
- This decision described the pyc4.0 direction. pyCircuit 6 restores the global
  cycle-aware signal model as the primary authoring contract. See Decision 0148.

## Decision 0011: C++ emits one SimObject class per module type

**Status:** Accepted

**Context / Goal**
To scale functional simulation and keep DFX/probe mapping clean, we want strong
module boundaries (SimObjects) with explicit state ownership, while keeping
internal combinational logic flattenable.

**Decision**
- For each `@module` **type**, emit a dedicated C++ SimObject class (option A),
  e.g. `<ModuleName>_SimObject`.
- Each `@module` **instance** becomes an instance of that class in the
  instantiated object graph.

**Implications**
- The emitter must generate a module-type class definition and a top-level
  builder/instantiation graph.
- DFX paths align naturally with instance hierarchy.

**Source**
- User direction in #linx-core (2026-03-01): "a" for per-module-type class emission.

## Decision 0012: Parent SimObjects own children via unique_ptr (collections via vector)

**Status:** Accepted

**Context / Goal**
We need a clear ownership model for a large SimObject hierarchy without
reference-count overhead or ambiguous lifetimes.

**Decision**
- Parent SimObjects own child SimObjects via `std::unique_ptr` (option A).
- Child collections are represented with `std::vector<std::unique_ptr<...>>` (or
  equivalent) when multiplicity is dynamic.

**Implications**
- Object graph lifetime is tree-owned from the top.
- Avoids `shared_ptr` overhead and simplifies teardown.

**Source**
- User direction in #linx-core (2026-03-01): "a" for unique_ptr ownership.

## Decision 0013: C++ runtime is header + precompiled library

**Status:** Accepted

**Context / Goal**
Header-only template-heavy runtimes can cause compile-time blow-ups for ultra-
large generated designs. We want fast incremental builds and stable runtime
behavior.

**Decision**
- The pyc4.0 C++ runtime will be delivered as **headers + a precompiled library**
  (static and/or shared) (option B).

**Implications**
- Build system must produce and link a precompiled runtime library. The active
  pyCircuit 6 library is `libpyc6_runtime`; the earlier `libpyc4_runtime`
  working name is superseded by Decision 0149.
- Public headers must keep ABI boundaries clean and minimize template bloat.

**Source**
- User direction in #linx-core (2026-03-01): "b" for runtime delivery form.

## Decision 0014: C++ runtime depends only on STL by default

**Status:** Accepted

**Context / Goal**
Minimize dependency footprint and maximize portability across build
environments. Keep the runtime easy to integrate and avoid dependency/version
conflicts.

**Decision**
- The pyc4.0 C++ runtime will depend on **STL only** by default (option A).
- Non-STL dependencies (fmt/spdlog/etc.) may exist only as optional features.

**Implications**
- Logging and formatting must have a minimal default implementation.
- Optional dependency features must be cleanly gated.

**Source**
- User direction in #linx-core (2026-03-01): "a" for STL-only default.

## Decision 0015: Execution is single-thread deterministic first; parallel hooks later

**Status:** Accepted

**Context / Goal**
We need a correct, deterministic baseline simulator for bring-up and debugging.
Ultra-large performance can be improved later once semantics, DFX, and runtime
stability are proven.

**Decision**
- Start with **single-thread deterministic** scheduling/execution (option A).
- Design APIs to allow adding parallelism later (hooks / partitions / task graph),
  without changing architectural semantics.

**Implications**
- Trace/probe behavior is deterministic and reproducible.
- Parallel implementation can be staged once the baseline is stable.

**Source**
- User direction in #linx-core (2026-03-01): "a" for single-thread first.

## Decision 0016: Trace output is primarily a binary event stream

**Status:** Accepted

**Context / Goal**
Ultra-large designs produce enormous trace volumes. Text-first formats are too
slow and too large. We need a scalable representation that supports efficient
writing and offline post-processing.

**Decision**
- Trace output is primarily a **binary event stream** (option A), written in
  chunks/streams.
- Offline tools can convert to human-readable views.

**Implications**
- Define a stable on-disk event schema and versioning.
- Provide minimal tooling for decoding/inspection.

**Source**
- User direction in #linx-core (2026-03-01): "a" for binary-first trace.

## Decision 0017: Path shortening uses short_name first, then stable hashing if still too long

**Status:** Accepted

**Context / Goal**
We require hierarchical paths for DFX/probes (Decision 0002) but must avoid
unwieldy long strings in deep hierarchies.

**Decision**
- Use combined strategy (option C):
  1) Prefer per-module/per-instance `short_name` (alias) when provided.
  2) If the path still exceeds configured depth/length thresholds, apply stable
     hashing to elide middle segments while preserving head/tail readability.

**Implications**
- Define canonical hash algorithm and threshold policy.
- Ensure the shortened path remains stable across builds given identical
  hierarchy + naming.

**Source**
- User direction in #linx-core (2026-03-01): "c" for short_name + hash.

## Decision 0018: ProbeRegistry uses both string paths and numeric ids

**Status:** Accepted

**Context / Goal**
We need human-friendly probe addressing (paths) and high-performance runtime
lookup (ids). Tooling integration also benefits from stable identifiers.

**Decision**
- ProbeRegistry maintains dual indexing (option A):
  - canonical `path` string (for user/tool interaction)
  - numeric `probe_id` (e.g. `u64`) for fast runtime access

**Implications**
- Define `probe_id` stability rules (within a run vs across builds).
- Ensure path shortening (Decision 0017) is reflected in registry path keys.

**Source**
- User direction in #linx-core (2026-03-01): "a" for path+id dual indexing.

## Decision 0019: probe_id is stable across builds for the same canonical path

**Status:** Accepted

**Context / Goal**
Tooling (trace correlation, external dashboards, regression comparison) benefits
from stable identifiers. If ids change across builds, cached views and
annotations become invalid.

**Decision**
- `probe_id` must be **stable across builds** (option B) for the same canonical
  probe path.

**Implications**
- Define deterministic id assignment (e.g. hash(canonical_path) with versioned
  algorithm/salt).
- Renames/path shortening changes will change ids; provide migration guidance.

**Source**
- User direction in #linx-core (2026-03-01): "b" for cross-build stable probe_id.

## Decision 0020: probe_id is hash64(canonical_path)

**Status:** Accepted

**Context / Goal**
To ensure cross-build stable identifiers (Decision 0019) with minimal
implementation complexity, ids should be derived deterministically from the
canonical probe path.

**Decision**
- `probe_id = hash64(canonical_path)` (option A), using a versioned, specified
  hash algorithm.

**Implications**
- Specify the exact algorithm (e.g. xxHash64 / SipHash / HighwayHash) and
  endianness.
- Provide collision detection/handling strategy (log + secondary disambiguator).

**Source**
- User direction in #linx-core (2026-03-01): "a" for hash64(path).

## Decision 0021: hash64 algorithm is xxHash64

**Status:** Accepted

**Context / Goal**
We need a fast, deterministic 64-bit hash for large numbers of probes/paths.

**Decision**
- `hash64` uses **xxHash64** (option A), with a specified seed (default 0 unless
  otherwise required).

**Implications**
- Vendor a small xxHash implementation or add it as an optional runtime feature
  (but keep Decision 0014: STL-only default; thus prefer vendoring).

**Source**
- User direction in #linx-core (2026-03-01): "a" for xxHash64.

## Decision 0022: Detect hash collisions; resolve by rehashing with a numeric suffix

**Status:** Accepted

**Context / Goal**
Even with 64-bit hashes, collisions are possible. We need a deterministic,
visible strategy that preserves usability and avoids silent misbinding.

**Decision**
- At registry construction, detect collisions.
- If collision occurs, resolve deterministically by rehashing:
  - try `hash64(path + "#1")`, then `#2`, ... until a free id is found (option B).

**Implications**
- Log collisions with both paths and final assigned ids.
- Stability across builds is preserved as long as the set/order of colliding
  paths is stable; specify tie-breaking order (e.g. sort by path string).

**Source**
- User direction in #linx-core (2026-03-01): "b" for collision rehashing.

## Decision 0023: canonical_path format is <instance_path>:<field_path>

**Status:** Accepted

**Context / Goal**
`canonical_path` is the user-facing identity string and the input to `probe_id`
(Decisions 0019–0022). It must be unambiguous and easy to parse.

**Decision**
- `canonical_path = <canonical_instance_path> ":" <canonical_field_path>` (option A).

**Implications**
- Instance paths use the shortening rules (Decision 0017).
- Field paths follow the canonical field path rules (Decision 0009).

**Source**
- User direction in #linx-core (2026-03-01): "a" for instance:field separator.

## Decision 0024: Array indexing in paths uses square brackets

**Status:** Accepted

**Context / Goal**
Canonical path strings must encode array indexing unambiguously and in a
familiar style.

**Decision**
- Array indices use `name[index]` (option A), e.g. `foo[3]`.

**Implications**
- Specify escaping rules if field names may contain `[`/`]` (ideally forbid in
  identifiers).

**Source**
- User direction in #linx-core (2026-03-01): "a" for `foo[3]` indexing.

## Decision 0025: Path identifiers use strict C-like character set; no escaping

**Status:** Accepted

**Context / Goal**
Canonical paths should be trivial to parse and stable across tooling. Escaping
rules add ambiguity and implementation burden.

**Decision**
- Identifiers in paths are restricted to a strict C-like set (option A):
  - `[A-Za-z_][A-Za-z0-9_]*`
- No escaping/encoding is supported in canonical paths.

**Implications**
- Frontend must validate and reject identifiers outside this set (or map them to
  `short_name`/aliases that obey the restriction).

**Source**
- User direction in #linx-core (2026-03-01): "a" for strict identifiers.

## Decision 0026: Simulation cycle uses 2-phase execution (comb then tick/commit)

**Status:** Accepted

**Context / Goal**
We need clear, deterministic clocked semantics with explicit separation between
combinational evaluation and state updates, to match hardware intuition and
avoid accidental read-after-write within a cycle.

**Decision**
- Use 2-phase cycle semantics (option A):
  - `comb()` computes purely combinational outputs from current state/inputs.
  - `tick()` computes next-state from current state and comb results.
  - `commit()` applies next-state (may be explicit or an internal step).

**Implications**
- Generated code must maintain `state` vs `next_state` separation.
- Tracing can cleanly attribute events to comb vs tick.

**Source**
- User direction in #linx-core (2026-03-01): "a" for 2-phase cycle.

## Decision 0027: Provide both step() and explicit comb/tick/commit APIs

**Status:** Accepted

**Context / Goal**
Most users want a simple `step()` API, but advanced testing/DFX may require
manual phase control.

**Decision**
- Provide both (option C):
  - Default high-level API: `step()` performs comb + tick + commit.
  - Advanced APIs: `comb()`, `tick()`, `commit()` are also exposed.

**Implications**
- Document legal call sequences and invariants.
- Ensure trace/probe semantics are well-defined for both modes.

**Source**
- User direction in #linx-core (2026-03-01): "c" for step + explicit phase APIs.

## Decision 0028: Reset is modeled as an input affecting tick; provide reset() helper

**Status:** Accepted

**Context / Goal**
Keep reset semantics consistent with clocked state update rules and avoid hidden
state mutations outside the comb/tick/commit pipeline.

**Decision**
- Model reset as an input that affects `tick()` next-state computation (option A).
- Provide a `reset()` helper that asserts reset and steps as required.

**Implications**
- Reset behavior is traceable and deterministic.
- Avoids bypassing commit invariants.

**Source**
- User direction in #linx-core (2026-03-01): "a" for modeled reset + helper.

## Decision 0029: Simulation timebase is cycle count (u64)

**Status:** Accepted

**Context / Goal**
We need a simple, deterministic time representation aligned with the step-based
execution model.

**Decision**
- Use cycle count as the timebase: `time = u64 cycles` (option A).

**Implications**
- Trace timestamps and events are indexed by cycle.
- Physical time (ns/ps) can be derived externally if a clock period is known.

**Source**
- User direction in #linx-core (2026-03-01): "a" for cycle-based time.

## Decision 0030: Trace timestamps include phase (cycle, phase)

**Status:** Accepted

**Context / Goal**
With multi-phase simulation (Decision 0026) and optional explicit phase APIs
(Decision 0027), trace needs to preserve intra-cycle ordering.

**Decision**
- Trace timestamps include phase information (option A):
  - `timestamp = (cycle, phase)` where `phase ∈ {comb, tick, commit}`.

**Implications**
- Event schema must encode phase.
- When using `step()`, phases are emitted in the standard order.

**Source**
- User direction in #linx-core (2026-03-01): "a" for (cycle, phase) timestamps.

## Decision 0031: Trace event minimal set includes cycle boundaries, value changes, and log/assert

**Status:** Accepted

**Context / Goal**
Binary trace needs a minimal closed set of events to support basic waveform-like
inspection and debugging without overengineering.

**Decision**
- Use minimal event set (option A):
  - `CycleBegin` / `CycleEnd` (with timestamp)
  - `ValueChange(probe_id, value)`
  - `Log` / `Assert` events

**Implications**
- Registry/probe declarations are separate from the event stream initially.
- Additional events can be added in future versions with schema versioning.

**Source**
- User direction in #linx-core (2026-03-01): "a" for minimal trace event set.

## Decision 0032: ValueChange value encoding is unified bitvector (bits + width)

**Status:** Accepted

**Context / Goal**
Trace values must represent both scalars and wide vectors with a single
consistent encoding.

**Decision**
- Use unified encoding (option A): `value = (width, bits)` where scalars are
  represented as bitvectors with `width <= 64`.

**Implications**
- Define bit ordering (LSB0) and byte order for serialization.
- Optionally add future fast-path scalar tags as an extension without changing
  semantic meaning.

**Source**
- User direction in #linx-core (2026-03-01): "a" for unified bitvector encoding.

## Decision 0033: Bitvector serialization uses LSB0 and little-endian byte order

**Status:** Accepted

**Context / Goal**
To ensure cross-language, cross-platform consistency for trace value decoding,
we must define bit ordering and byte order.

**Decision**
- Bit ordering is **LSB0**: bit 0 is the least-significant bit.
- Byte order is **little-endian** in the serialized byte stream (option A).

**Implications**
- Readers must reconstruct integers/vectors accordingly.
- For widths not multiple of 8, high unused bits in the final byte are ignored
  (must be zeroed on write).

**Source**
- User direction in #linx-core (2026-03-01): "a" for LSB0 + little-endian.

## Decision 0034: ValueChange emission is delta-by-default with optional full/periodic dumps

**Status:** Accepted

**Context / Goal**
Emitting all probe values every cycle does not scale. We need a default encoding
that compresses well while still allowing periodic resynchronization.

**Decision**
- Default: emit `ValueChange` events only when the value changes (option A).
- Support optional modes:
  - full dump (emit all watched probes at a chosen point)
  - periodic dump (emit full dump every N cycles)

**Implications**
- Trace readers must maintain last-value state per probe.
- Periodic/full dumps provide recovery points for seeking.

**Source**
- User direction in #linx-core (2026-03-01): "a" for delta encoding default.

## Decision 0035: Log/Assert event schema uses simple levels and messages

**Status:** Accepted

**Context / Goal**
We need debuggable, filterable textual diagnostics in the trace stream without
overcomplicating the event schema.

**Decision**
- Use simple event forms (option A):
  - `Log(level, message)`
  - `Assert(message, fatal)`

**Implications**
- Define `level` enum (e.g. debug/info/warn/error).
- `Assert(fatal=true)` may terminate the simulation or mark the trace as
  aborted.

**Source**
- User direction in #linx-core (2026-03-01): "a" for Log(level)/Assert(fatal).

## Decision 0036: Log level enum is trace/debug/info/warn/error/fatal

**Status:** Accepted

**Context / Goal**
Provide enough granularity for filtering diagnostics without making the logging
system heavyweight.

**Decision**
- Log levels are (option B):
  - `trace`, `debug`, `info`, `warn`, `error`, `fatal`

**Implications**
- `fatal` is distinct from `Assert(fatal=true)`; `fatal` logs may still be used
  for non-assert termination paths.

**Source**
- User direction in #linx-core (2026-03-01): "b" for 6-level enum.

## Decision 0037: Support both self-describing and external-manifest trace modes

**Status:** Accepted

**Context / Goal**
Single-file traces are easier to share and analyze, but separating the probe
manifest can reduce duplication and file size.

**Decision**
- Support both (option C):
  - Self-describing mode: include `ProbeDeclare` records in the trace.
  - External-manifest mode: keep probe registry/manifest as a separate artifact.

**Implications**
- Define a canonical manifest schema shared by both modes.
- Readers must handle either embedded declarations or an external manifest.

**Source**
- User direction in #linx-core (2026-03-01): "c" for dual trace modes.

## Decision 0038: ProbeDeclare includes id/path/kind/type plus optional human alias

**Status:** Accepted

**Context / Goal**
We want self-describing traces that are usable by humans without requiring a
separate UI-side name mapping.

**Decision**
- `ProbeDeclare` includes (option B):
  - `probe_id`
  - `canonical_path`
  - `kind`
  - `type_sig`
  - optional `human_name` / alias

**Implications**
- Define `type_sig` encoding to cover Bits/Array/Struct/Union.
- Alias is non-unique and for display only; `canonical_path` remains the stable
  identity.

**Source**
- User direction in #linx-core (2026-03-01): "b" for adding alias.

## Decision 0039: type_sig reuses spec type structure with compact binary encoding

**Status:** Accepted

**Context / Goal**
Trace readers must be able to reconstruct structured types for display and
navigation. A compact, versionable binary encoding avoids heavy text parsing.

**Decision**
- Encode `type_sig` by reusing the spec type structure (option A):
  - `Bits / Array / Struct / Union / Signature`
- Use a compact binary variant/TLV encoding with schema versioning.

**Implications**
- Define a stable type-tag enum and recursive encoding rules.
- Readers can decode without needing the original Python source.

**Source**
- User direction in #linx-core (2026-03-01): "a" for structured binary type_sig.

## Decision 0040: Trace has a global schema_version; type_sig does not carry per-record version

**Status:** Accepted

**Context / Goal**
Avoid per-record overhead while keeping decoding rules unambiguous.

**Decision**
- Store a global `schema_version` in the trace header (option A).
- `ProbeDeclare.type_sig` is interpreted under that global version.

**Implications**
- Any breaking changes require bumping the trace schema version.
- Readers can reject unsupported versions early.

**Source**
- User direction in #linx-core (2026-03-01): "a" for global schema_version.

## Decision 0041: Trace framing is chunked records with length/type/payload and optional CRC

**Status:** Accepted

**Context / Goal**
We need forward compatibility (skip unknown record types), robustness, and the
ability to seek/partition traces.

**Decision**
- Use chunked framing (option A):
  - `[chunk_len][chunk_type][payload][crc?]...`

**Implications**
- Define chunk_type namespace and rules for unknown chunk skipping.
- CRC can be optional per chunk or globally configured.

**Source**
- User direction in #linx-core (2026-03-01): "a" for chunked framing.

## Decision 0042: chunk_len is fixed u32 little-endian

**Status:** Accepted

**Context / Goal**
Keep decoding fast and simple; 4GB max chunk size is more than sufficient.

**Decision**
- Encode `chunk_len` as fixed-width `u32` little-endian (option A).

**Implications**
- Writer must split very large payloads into multiple chunks.

**Source**
- User direction in #linx-core (2026-03-01): "a" for u32 LE chunk_len.

## Decision 0043: No per-chunk CRC in trace framing

**Status:** Accepted

**Context / Goal**
Avoid overhead and keep the format minimal; rely on transport/storage integrity
or higher-level validation.

**Decision**
- Do not include per-chunk CRC (option C).

**Implications**
- Corruption may manifest as decode errors later; readers should still validate
  lengths and handle malformed input robustly.

**Source**
- User direction in #linx-core (2026-03-01): "c" for no CRC.

## Decision 0044: Optional chunk-level compression (zstd) for trace payloads

**Status:** Accepted

**Context / Goal**
Large traces benefit greatly from compression. Chunk-level compression preserves
seeking and partial decoding while keeping the core framing stable.

**Decision**
- Support optional chunk-level compression (option B):
  - Introduce a `Compressed` chunk type whose payload contains:
    - compression algorithm (default: zstd)
    - uncompressed length
    - compressed bytes

**Implications**
- Readers must be able to skip compressed chunks if unsupported.
- Writers choose chunk sizes to balance compression ratio and random access.

**Source**
- User direction in #linx-core (2026-03-01): "b" for chunk-level compression.

## Decision 0045: Default compressed chunk size is 1MB with zstd level 3

**Status:** Accepted

**Context / Goal**
Pick sensible defaults that balance compression ratio, CPU cost, and random
access granularity.

**Decision**
- Defaults (option A):
  - uncompressed chunk target size: **1MB**
  - zstd compression level: **3**

**Implications**
- Expose overrides for power users and CI.

**Source**
- User direction in #linx-core (2026-03-01): "a" for 1MB + level 3.

## Decision 0046: Only CycleBegin/End carry timestamps; other events inherit current time

**Status:** Accepted

**Context / Goal**
Reduce trace size while keeping event ordering well-defined.

**Decision**
- Only `CycleBegin` / `CycleEnd` carry `timestamp=(cycle,phase)` (option A).
- All other events are interpreted at the current active timestamp.

**Implications**
- Readers maintain a "current timestamp" state.
- Writers must emit CycleBegin/End in a consistent order to avoid ambiguity.

**Source**
- User direction in #linx-core (2026-03-01): "a" for implicit timestamps.

## Decision 0047: CycleBegin/End are emitted per phase (comb/tick/commit)

**Status:** Accepted

**Context / Goal**
Make phase boundaries explicit in the trace stream so tools can segment events
and present phase-scoped views.

**Decision**
- Emit `CycleBegin`/`CycleEnd` per phase (option A):
  - `(cycle, comb)` begin/end
  - `(cycle, tick)` begin/end
  - `(cycle, commit)` begin/end

**Implications**
- Writers must follow a consistent ordering of phases.
- Readers can treat begin/end as the scope for inherited timestamps.

**Source**
- User direction in #linx-core (2026-03-01): "a" for per-phase boundaries.

## Decision 0048: Default ValueChange sampling/emission occurs in comb phase (end-of-comb)

**Status:** Accepted

**Context / Goal**
Provide a predictable, intuitive default for waveform-like visualization of
combinational results each cycle.

**Decision**
- Default: emit/sample `ValueChange` in the **comb** phase (option A), scoped
  within the `(cycle, comb)` begin/end interval (typically near end-of-comb).

**Implications**
- Other views (tick/commit state) can be obtained by probing state elements and
  emitting in corresponding phases if desired.

**Source**
- User direction in #linx-core (2026-03-01): "a" for comb-phase default.

## Decision 0049: Reg module write semantics: enable-gated write, otherwise hold

**Status:** Accepted

**Context / Goal**
Define unambiguous state element behavior for tracing and commit semantics.

**Decision**
- For `reg`-like stateful modules with an `enable`:
  - if `enable==1`: write/update to the new value
  - if `enable==0`: **no write**; output/state **holds** its previous value

**Implications**
- With delta-only `ValueChange` (Decision 0034), cycles where `enable==0` will
  typically emit no value change for that reg (because the value is held).
- Trace tools can treat `enable` as the authoritative indicator of whether a
  state update occurred.

**Source**
- User direction in #linx-core (2026-03-01): reg.enable==0 inhibits write and holds value.

## Decision 0050: Commit phase emits ValueChange for stateful probes when updates occur

**Status:** Accepted

**Context / Goal**
Make architectural/state updates explicit at commit time while keeping combinational
signals in comb. Works naturally with enable-gated regs and delta-only encoding.

**Decision**
- Default (option B): in **commit** phase, emit `ValueChange` for **stateful probes**
  (e.g., regs/CSRs/state elements) when an update occurs.
  - If a reg has `enable==0`, it is a hold; no update and typically no emitted
    value change.

**Implications**
- Tools can show "what became architecturally visible" by focusing on commit.
- Writers should classify probes as stateful vs combinational (or infer from module type).

**Source**
- User direction in #linx-core (2026-03-01): "b" for commit emitting stateful updates.

## Decision 0051: Probe kind explicitly encodes stateful vs combinational classification

**Status:** Accepted

**Context / Goal**
Trace writers and tools need a reliable way to decide whether a probe represents
state (commit-visible) or combinational signals, without brittle inference.

**Decision**
- Encode the classification explicitly in `ProbeDeclare.kind` (option A):
  - e.g. `comb` vs `state` (naming TBD)

**Implications**
- Readers can filter and present commit vs comb views deterministically.
- Avoids coupling trace semantics to module-type inference.

**Source**
- User direction in #linx-core (2026-03-01): "a" for explicit kind.

## Decision 0052: Probe kind is comb/state with optional subkind (reg/mem/csr/etc.)

**Status:** Accepted

**Context / Goal**
Keep the core classification simple while allowing richer filtering/grouping in
tools when needed.

**Decision**
- Use a two-level scheme (option C):
  - `kind`: `comb` | `state`
  - optional `subkind`: e.g. `reg` | `mem` | `csr` | ...

**Implications**
- Subkind is advisory; semantics derive primarily from kind.
- Trace writers can omit subkind when unknown.

**Source**
- User direction in #linx-core (2026-03-01): "c" for kind+subkind.

## Decision 0053: Record explicit Write events; keep ValueChange delta-based

**Status:** Accepted

**Context / Goal**
We need a clear notion of "a write happened" independent of whether the data
actually changed, while preserving compact delta-only value encoding.

**Decision**
- Support both signals (option C):
  - Emit explicit state update intent as `Write`-like events (e.g., reg-write,
    mem-write, csr-write) when a write command/enable is asserted.
  - Continue to emit `ValueChange` only when the observable value changes (delta).

**Implications**
- Tools can count/inspect writes even when the value is unchanged.
- Writers need access to write intents (valid/enable/mask) for stateful modules.

**Source**
- User direction in #linx-core (2026-03-01): "c" for write events + delta ValueChange.

## Decision 0054: Use generic Write event with required subkind; fields interpreted by subkind

**Status:** Accepted

**Context / Goal**
Avoid an explosion of event types while still capturing the differences between
reg/mem/csr writes.

**Decision**
- Use a generic `Write` event (option C) with a required `subkind`:
  - `subkind`: `reg` | `mem` | `csr` | ...
- Interpret optional fields based on `subkind`:
  - `probe_id` (always)
  - `addr` (mem/csr if applicable)
  - `mask` (mem if applicable)
  - `data` (written data)
  - `meta` (optional)

**Implications**
- Decoders need a per-subkind schema table.
- Writers can omit irrelevant fields.

**Source**
- User direction in #linx-core (2026-03-01): "c" for generic Write + subkind.

## Decision 0055: Write.data and Write.mask use the same ValueBlob bytes encoding as ValueChange

**Status:** Accepted

**Context / Goal**
Keep encodings consistent across events and avoid per-event special cases.

**Decision**
- Encode `Write.data` as `ValueBlob` bytes (same rules as `ValueChange.value`).
- Encode `Write.mask` also as bytes (`ValueBlob`), where the interpretation is
  subkind-defined (e.g., mem byte-enable mask).

**Implications**
- Readers can reuse the same value decoding pipeline.
- Mask semantics must be specified per subkind (width/endianness).

**Source**
- User direction in #linx-core (2026-03-01): "a" for ValueBlob encoding.

## Decision 0056: Define X-state (unknown) via an explicit known-mask alongside value bytes

**Status:** Accepted

**Context / Goal**
We need a first-class way to represent "unknown / uninitialized / unresolved"
values (X) in traces and intermediate representations, without overloading
normal numeric encodings.

**Decision**
- Support **bit-level** X (option A).
- Extend `ValueBlob` conceptually to carry two equal-length byte arrays:
  - `value_bytes`: the 0/1 payload bits
  - `known_mask_bytes`: 1 means the corresponding bit is known; 0 means **X**
- A bit is interpreted as:
  - if `known_mask=1`: bit value is `value_bytes` (0 or 1)
  - if `known_mask=0`: bit value is **X** (unknown)

**Notes**
- This is a 2-state value + 1-bit validity mask representation (common in RTL sims).
- For scalar bool: `known_mask=0` represents X.

**Implications**
- Trace consumers can render X cleanly and propagate unknowns when doing derived views.
- Writers that do not model X can set `known_mask_bytes` to all-ones.

**Source**
- User direction in #linx-core (2026-03-01): need an X-state definition; chose "A" for bit-level X.

## Decision 0057: Write.mask default is all-ones (full write)

**Status:** Accepted

**Context / Goal**
Choose an unsurprising default for write masks.

**Decision**
- If `Write.mask` is omitted, it is interpreted as **all-ones** (option A), i.e.
  full write over the addressed width.

**Implications**
- Writers only need to include mask for partial writes.

**Source**
- User direction in #linx-core (2026-03-01): "A" for default full-write mask.

## Decision 0058: Delta ValueChange triggers on changes to either value bytes or known-mask bytes

**Status:** Accepted

**Context / Goal**
Ensure X->known and known->X transitions are observable in delta-only traces.

**Decision**
- For delta emission, treat the pair `(value_bytes, known_mask_bytes)` as the
  logical value (option A).
- Emit `ValueChange` if either `value_bytes` **or** `known_mask_bytes` differs
  from the last emitted state.

**Implications**
- Trace size may increase slightly when unknowns resolve, but semantics are correct.

**Source**
- User direction in #linx-core (2026-03-01): "A" for including known-mask changes.

## Decision 0059: Partial writes preserve old value/known-mask for untouched bits/bytes

**Status:** Accepted

**Context / Goal**
Define deterministic merging behavior for masked writes in the presence of X-state.

**Decision**
- For masked/partial writes (option A):
  - written lanes update `value_bytes` and `known_mask_bytes` per the write data
  - **unwritten** lanes keep their previous `value_bytes` and `known_mask_bytes`

**Implications**
- Unknowns are not introduced unless explicitly written as unknown.

**Source**
- User direction in #linx-core (2026-03-01): "A" for preserving untouched lanes.

## Decision 0060: Write.data supports X via the same (value, known-mask) representation

**Status:** Accepted

**Context / Goal**
Allow a write to explicitly introduce unknown bits (X), and keep encoding uniform
across ValueChange and Write.

**Decision**
- Option A: `Write.data` uses the same `(value_bytes, known_mask_bytes)`
  representation as `ValueChange`.
  - `known_mask=0` bits mean the write sets those bits to X.

**Implications**
- Masked writes combine with Decision 0059: only written lanes apply the new
  known-mask; untouched lanes retain their old known-mask.

**Source**
- User direction in #linx-core (2026-03-01): "A" for Write supporting X.

## Decision 0061: Uninitialized state defaults to X until reset/init/write defines it

**Status:** Accepted

**Context / Goal**
Model realistic power-on/unknown state to avoid masking bugs and to make X
resolution visible in traces.

**Decision**
- Option A: for stateful elements, the default power-on/uninitialized value is
  **all X** (`known_mask_bytes` all-zeros) until:
  - a reset/init sequence defines it, or
  - a write defines it (possibly partially, per mask)

**Implications**
- Consumers should expect early-cycle X noise unless reset is modeled.
- Reset events (if present) should drive known_mask to 1 for reset-defined bits.

**Source**
- User direction in #linx-core (2026-03-01): "A" for default uninitialized = X.

## Decision 0062: Include explicit Reset events in the trace

**Status:** Accepted

**Context / Goal**
Make reset sequences explicitly visible and distinguishable from ordinary writes.

**Decision**
- Option A: include a `Reset` event, with fields along the lines of:
  - `domain` (optional)
  - `kind` (optional: e.g., POR, warm, SW)
  - `cycle` and `phase`
- Reset-driven state updates should appear as commit-phase stateful `ValueChange`
  (per Decision 0050).

**Implications**
- Tools can segment timelines and suppress/annotate early X-resolution noise.

**Source**
- User direction in #linx-core (2026-03-01): "A" for explicit Reset event.

## Decision 0063: Reset is represented as edge events (assert/deassert)

**Status:** Accepted

**Context / Goal**
Represent reset in a streaming-friendly way with unambiguous duration.

**Decision**
- Option A: represent reset as two edge events:
  - `ResetAssert{domain?, kind?, cycle, phase}`
  - `ResetDeassert{domain?, kind?, cycle, phase}`

**Implications**
- Writers can emit events online without knowing the future.
- Readers can reconstruct reset intervals precisely.

**Source**
- User direction in #linx-core (2026-03-01): "A" for assert/deassert edges.

## Decision 0064: Reset domain is required (module/domain scoped resets)

**Status:** Accepted

**Context / Goal**
Flush/reset may be applied to a specific module (or reset domain), not
necessarily the whole design.

**Decision**
- Option A: `domain` is **required** on reset edge events.
  - `ResetAssert{domain, kind?, cycle, phase}`
  - `ResetDeassert{domain, kind?, cycle, phase}`

**Implications**
- Tools can distinguish global resets from per-module flushes (modeled as reset).
- Writers must define a stable domain naming/ID scheme.

**Source**
- User direction in #linx-core (2026-03-01): "A" because flushing a module uses reset semantics.

## Decision 0065: Reset.kind is standardized as an enum including flush

**Status:** Accepted

**Context / Goal**
Provide a uniform way for tools to distinguish POR/warm reset vs module flushes.

**Decision**
- Option A: standardize `kind` as a small enum (extensible), at least:
  - `por`
  - `warm`
  - `flush`
  - `sw`

**Implications**
- Tools can consistently color/segment timelines by reset kind.
- Writers should map internal reset causes into this enum.

**Source**
- User direction in #linx-core (2026-03-01): "A" for kind enum.

## Decision 0066: Reset effects are represented via commit-phase ValueChange events

**Status:** Accepted

**Context / Goal**
Avoid a special bulk snapshot format and keep all state updates flowing through
one mechanism.

**Decision**
- Option A: reset results are expressed via commit-phase `ValueChange` events
  (including `known_mask` transitions), rather than embedding a bulk snapshot in
  the reset event.

**Implications**
- Tools that already understand ValueChange automatically display reset effects.
- Reset event remains a timeline marker, not a state container.

**Source**
- User direction in #linx-core (2026-03-01): "A" for reset effects via ValueChange.

## Decision 0067: Flush/reset includes explicit invalidate events for in-flight work

**Status:** Accepted

**Context / Goal**
When a module is flushed (modeled as reset), it often cancels in-flight
transactions/queue entries. We want that cancellation to be explicit in the
trace so tools do not need heuristics.

**Decision**
- Option A: introduce an explicit invalidation/cancel event, e.g.:
  - `Invalidate{domain, reason?, cycle, phase, scope?}`
- Emit this around the flush/reset boundary to indicate which in-flight work is
  being dropped.

**Implications**
- Trace viewers can visually strike-through or terminate flows at invalidate.
- Writers must define `scope`/`reason` conventions per domain.

**Source**
- User direction in #linx-core (2026-03-01): "A" for explicit invalidate events.

## Decision 0068: Invalidate supports both domain-wide and fine-grained scope (scope optional)

**Status:** Accepted

**Context / Goal**
We want a streaming-friendly invalidate event that is useful immediately (coarse
flush) but can scale to precise cancellation when IDs exist.

**Decision**
- Choose option C: `Invalidate.scope` is **optional**.
  - If `scope` is omitted: invalidate applies to the entire `domain`.
  - If `scope` is present: it precisely identifies the object(s) to cancel
    (queue/entry id, txn id, ROB id, etc.; domain-defined).

**Implications**
- Minimal writers can emit coarse invalidates without schema gymnastics.
- Advanced writers can emit targeted invalidates for better visualization.

**Source**
- Assistant decision in #linx-core (2026-03-01) after user: "you decide encoding".

## Decision 0069: Invalidate is emitted in pre phase (before reset and commit state updates)

**Status:** Accepted

**Context / Goal**
Provide a deterministic ordering: first cancel in-flight work, then perform
reset/flush markers, then commit the resulting architectural state changes.

**Decision**
- Option A: emit `Invalidate` in `pre` phase.
  - Typical ordering within a cycle:
    1) `Invalidate` (pre)
    2) `ResetAssert` / `ResetDeassert` (pre)
    3) commit-phase stateful `ValueChange`

**Implications**
- Viewers can terminate flows before showing reset-driven state changes.
- Writers have a clear phase to attach cancellation semantics.

**Source**
- User direction in #linx-core (2026-03-01): "A" for pre-phase invalidate.

## Decision 0070: Invalidate.reason is required

**Status:** Accepted

**Context / Goal**
Provide a stable classification key so tools can group/visualize different
cancellation causes.

**Decision**
- Option A: `Invalidate.reason` is **required**.
  - Examples: `flush_mispredict`, `flush_exception`, `replay`, `squash`, etc.

**Implications**
- Writers must map internal cancel causes to a stable reason string/enum.
- Viewers can color/aggregate invalidations by reason.

**Source**
- User direction in #linx-core (2026-03-01): "A" for required reason.

## Decision 0071: Invalidate.reason uses a standardized enum (with extensible other)

**Status:** Accepted

**Context / Goal**
Avoid taxonomy drift across writers; enable consistent viewer behavior.

**Decision**
- Option A: `Invalidate.reason` is standardized as an enum.
  - Any out-of-tree reason should use an escape hatch like `other:<string>`
    (or an `OTHER` + `detail` scheme in the future).

**Implications**
- Viewers can reliably color/aggregate by reason.
- Writers have to update the enum (or use other:*) for new reason classes.

**Source**
- User direction in #linx-core (2026-03-01): "A" for reason enum.

## Decision 0072: Event cycle is explicit and required

**Status:** Accepted

**Context / Goal**
Keep parsing and random access simple and deterministic across tooling.

**Decision**
- Option A: every event carries an explicit `cycle` (required).

**Implications**
- Slightly larger traces, but much simpler parsing/indexing and slicing.

**Source**
- User direction in #linx-core (2026-03-01): "A" for explicit required cycle.

## Decision 0073: phase is optional; default phase is commit

**Status:** Accepted

**Context / Goal**
Reduce verbosity for the common case where most events are commit-phase.

**Decision**
- Option B: `phase` is optional.
  - If `phase` is omitted, it defaults to `commit`.

**Implications**
- Writers must explicitly set `phase=pre` for pre-phase events.
- Readers must apply the defaulting rule consistently.

**Source**
- User direction in #linx-core (2026-03-01): "B" for phase default=commit.

## Decision 0074: Emit explicit CycleEnd boundary events

**Status:** Accepted

**Context / Goal**
Make streaming viewers simpler by providing an unambiguous “end of cycle” marker
so UIs can flush/render without buffering heuristics.

**Decision**
- Option A: emit a boundary event at the end of each cycle, e.g.:
  - `CycleEnd{cycle}`

**Implications**
- Slight trace size overhead (one small event per cycle).
- Viewers can incrementally render per-cycle without guessing.

**Source**
- User direction in #linx-core (2026-03-01): "A" for explicit cycle boundary.

## Decision 0075: CycleEnd is global (no domain)

**Status:** Accepted

**Context / Goal**
Keep the cycle boundary semantics single and unambiguous; domain-specific
semantics are expressed via per-event `domain` fields instead.

**Decision**
- Option A: `CycleEnd` does **not** carry `domain`; it is a global timeline
  boundary.

**Implications**
- Multi-domain systems must share a common cycle axis for the trace.

**Source**
- User direction in #linx-core (2026-03-01): "A" for global CycleEnd.

## Decision 0076: Enforce within-cycle ordering: pre events, then commit, then CycleEnd

**Status:** Accepted

**Context / Goal**
Make traces deterministic for streaming viewers without requiring a secondary
stable sort key.

**Decision**
- Option A: within a given `cycle`, event order is constrained as:
  1) all `phase=pre` events (e.g. `Invalidate`, `ResetAssert`, `ResetDeassert`)
  2) all `phase=commit` events (including defaulted commit events)
  3) `CycleEnd{cycle}` as the final event for that cycle

**Implications**
- Writers must buffer/reorder within a cycle to satisfy ordering if needed.
- Readers can process events in file order.

**Source**
- User direction in #linx-core (2026-03-01): "A" for strict within-cycle ordering.

## Decision 0077: Pre-phase ordering is fixed: Invalidate → ResetAssert → ResetDeassert

**Status:** Accepted

**Context / Goal**
Provide deterministic semantics when multiple pre events occur in the same
cycle.

**Decision**
- Option A: within `phase=pre` for a cycle, enforce this order:
  1) `Invalidate`
  2) `ResetAssert`
  3) `ResetDeassert`

**Implications**
- Writers may need to buffer pre events within a cycle.
- Readers/viewers can rely on a consistent causal timeline.

**Source**
- User direction in #linx-core (2026-03-01): "A" for fixed pre ordering.

## Decision 0078: Allow same-cycle reset pulses (assert+deassert in one cycle)

**Status:** Accepted

**Context / Goal**
Support 1-cycle reset/flush pulses without introducing a dedicated pulse event.

**Decision**
- Option A: allow `ResetAssert` and `ResetDeassert` to both appear in the same
  `cycle` (in pre phase, respecting the pre-ordering rules).

**Implications**
- Readers should treat this as a single-cycle reset interval.

**Source**
- User direction in #linx-core (2026-03-01): "A" for same-cycle pulse.

## Decision 0079: Reset is non-reentrant per domain (strict pairing)

**Status:** Accepted

**Context / Goal**
Keep reset semantics simple and avoid having to model nesting depth.

**Decision**
- Option A: reset is non-reentrant for a given `domain`.
  - You must not emit a second `ResetAssert` for a domain that is already in the
    asserted state.

**Implications**
- Writers must coalesce overlapping causes into a single reset interval.
- Readers can treat reset as a boolean state per domain.

**Source**
- User direction in #linx-core (2026-03-01): "A" for non-reentrant reset.

## Decision 0080: Reset.kind is immutable while asserted; kind changes require deassert+assert

**Status:** Accepted

**Context / Goal**
Avoid introducing additional state-machine events and keep reset a simple boolean
interval with a stable classification.

**Decision**
- Choose option A: while a domain is in reset asserted state, `Reset.kind` does
  not change.
  - If a different reset cause/kind arises, writers must either:
    - coalesce it into the existing kind (policy-defined), or
    - end the current reset (`ResetDeassert`) and begin a new one (`ResetAssert`)
      with the new kind.

**Implications**
- No `ResetKindChange` event type needed.
- Viewers can treat kind as a stable tag for the reset interval.

**Source**
- Assistant recommendation in #linx-core (2026-03-01): pick A for kind immutability.

## Decision 0081: Remove Reset.kind=sw

**Status:** Accepted

**Context / Goal**
The `sw` label is ambiguous (software-triggered reset vs software-requested
flush) and overlaps conceptually with more explicit kinds.

**Decision**
- Option C: remove `Reset.kind=sw`.
  - Prefer using explicit kinds such as `flush` (pipeline flush semantics) or
    architectural reset kinds (e.g., por/warm) where applicable.

**Implications**
- Fewer ambiguous categories; tools/writers converge on shared semantics.

**Source**
- User direction in #linx-core (2026-03-01): "C" to drop sw.

## Decision 0082: Keep both Reset.kind=flush and Reset.kind=warm

**Status:** Accepted

**Context / Goal**
Distinguish pipeline/in-flight clearing from a broader but non-POR reset.

**Decision**
- Option A: keep both kinds.
  - `flush`: pipeline/in-flight work invalidation semantics.
  - `warm`: module/subsystem reset semantics that are stronger than flush but
    not a full power-on reset.

**Implications**
- Writers can choose the appropriate semantic level.
- Viewers can present flush vs warm differently.

**Source**
- User direction in #linx-core (2026-03-01): "A" to keep both.

## Decision 0083: warm reset should be accompanied by Invalidate (recommended, not required)

**Status:** Accepted

**Context / Goal**
Make in-flight cancellation explicit for viewers when warm resets occur, while
not forcing all writers to emit additional events.

**Decision**
- Option C: recommend emitting an `Invalidate` alongside `ResetAssert(kind=warm)`
  (same cycle or adjacent), but do not require it.
  - Linters/tools may warn if warm reset occurs without a corresponding
    invalidate.

**Implications**
- Minimal traces remain valid.
- High-fidelity traces can keep causal cancellation explicit.

**Source**
- User direction in #linx-core (2026-03-01): "C" for recommend-but-not-require.

## Decision 0084: flush reset should be accompanied by Invalidate (recommended, not required)

**Status:** Accepted

**Context / Goal**
Encourage explicit cancellation semantics for flush resets while preserving a
low-friction writer experience.

**Decision**
- Option C: recommend emitting an `Invalidate` alongside `ResetAssert(kind=flush)`
  (same cycle or adjacent), but do not require it.
  - Linters/tools may warn if flush reset occurs without a corresponding
    invalidate.

**Implications**
- Writers can start with reset-only traces.
- Viewers get best results when invalidate is present.

**Source**
- User direction in #linx-core (2026-03-01): "C" for recommend-but-not-require.

## Decision 0085: Coalescing events is a viewer presentation policy (not trace semantics)

**Status:** Accepted

**Context / Goal**
Keep the trace contract focused on deterministic semantics and ordering, while
allowing viewers to choose richer or simpler presentations without constraining
writers.

**Decision**
- Option C: the spec does not mandate whether viewers must show `Invalidate` and
  `ResetAssert(kind=flush)` separately or as a single “Flush” concept.
  - Viewers **may** coalesce related same-cycle events into a single UI concept
    (presentation layer), but the underlying events remain distinct.

**Implications**
- Trace format remains stable and minimal.
- Different viewers can optimize for clarity vs fidelity.

**Source**
- User direction in #linx-core (2026-03-01): "C" for viewer policy.

## Decision 0086: Define a normative Viewer Contract layer

**Status:** Accepted

**Context / Goal**
Ensure consistent user experience and interpretation across multiple viewers by
standardizing key presentation/interaction behaviors on top of the same trace
semantics.

**Decision**
- Option A: the spec is explicitly two-layered:
  - **Trace Semantics**: event meaning + ordering + decoding.
  - **Viewer Contract**: required/standard viewer behaviors (e.g., default
    aggregation rules, reset/flush visualization conventions, ordering within UI
    lanes).

**Implications**
- Viewer implementations become more interoperable.
- Some flexibility is traded for consistency across tools.

**Source**
- User direction in #linx-core (2026-03-01): "A" to define a viewer contract.

## Decision 0087: Viewer default is object-level timelines

**Status:** Accepted

**Context / Goal**
Optimize for signal-centric debugging (regs/mems/ops) where users track an
object’s evolution over time; cycle markers remain available as a global ruler.

**Decision**
- Option B: viewers default to **object-level** timelines; `cycle` is primarily a
  shared time axis, not the primary grouping unit.

**Implications**
- Viewers should provide optional cycle-level grouping/filters as secondary
  affordances.

**Source**
- User direction in #linx-core (2026-03-01): "B" for object-level default.

## Decision 0088: Boundary events render as global overlays by default

**Status:** Accepted

**Context / Goal**
Avoid cluttering per-object lanes while still making major boundaries obvious in
an object-centric timeline.

**Decision**
- Option A: `Reset*`, `Invalidate`, and similar boundary events should render as
  a **global overlay band** by default (spanning all lanes), rather than being
  injected into each object lane.

**Implications**
- Viewers must provide interaction affordances (hover/click) on the overlay to
  inspect affected domains/scopes.

**Source**
- User direction in #linx-core (2026-03-01): "A" for global overlay.

## Decision 0089: Use a single overlay lane; distinguish domains via styling

**Status:** Accepted

**Context / Goal**
Minimize vertical space usage while still supporting multi-domain boundaries.

**Decision**
- Option A: viewers use **one** overlay lane by default; different `domain`s are
  distinguished via color/labels/tooltips.

**Implications**
- Viewers should provide filtering/highlighting by domain for clarity when many
  domains are active.

**Source**
- User direction in #linx-core (2026-03-01): "A" for single overlay lane.

## Decision 0090: Viewer Contract focuses on large-design debugging, not pixel-perfect UI consistency

**Status:** Accepted

**Context / Goal**
pyc4.0’s primary goal is to make debugging **ultra-large** designs practical.
We want consistency where it improves interpretability, but we do not want to
freeze UI/interaction details that would slow iteration.

**Decision**
- Option A: Viewer Contract does **not** aim for pixel-level identical UI across
  viewers.
- Viewer Contract should standardize only what is necessary to make large-design
  debugging convenient and consistent at the semantic/information-architecture
  level (e.g., default layouts, required affordances, required interpretations).

**Implications**
- Different viewers may differ in exact visuals and interactions.
- Spec effort stays focused on scalability and debugging utility.

**Source**
- User direction in #linx-core (2026-03-01): "a 只要能够方便我们调试超超超大大大的design就好".

## Decision 0091: Debugging assumes selective instrumentation (not "probe everything")

**Status:** Accepted

**Context / Goal**
For ultra-large designs, we typically do not probe or view everything. The common
workflow is to add probes only at suspected/problematic areas, iterate, and keep
the system responsive.

**Decision**
- Option C: primary scalability concern is **hierarchy depth / pathing** and
  general navigability, assuming **selective probe insertion** rather than
  blanket probing/viewing of all signals.

**Implications**
- Probe/path naming must remain usable at extreme hierarchy depth.
- Tooling should optimize for rapid add/remove of targeted probes and quick
  navigation to those probes.

**Source**
- User direction in #linx-core (2026-03-01): "c 我们不会全部都probe和view。只是在有问题的地方插入probe".

## Decision 0092: Spec standardizes mechanisms; workflow remains non-normative

**Status:** Accepted

**Context / Goal**
Avoid over-specifying debugging workflows. Keep the spec focused on providing
robust mechanisms (probe/trace/path/id/semantics) that enable many workflows.
At the same time, provide guidance that helps humans debug ultra-large designs
quickly.

**Decision**
- The spec standardizes **mechanisms** (probe/trace/paths/ids and their
  semantics).
- Debugging workflow guidance and viewer UX flows are **not normative
  requirements**.

**Implications**
- Viewer implementations may offer different workflows as long as the core
  semantics remain interoperable.

**Source**
- User direction in #linx-core (2026-03-01): "a".

## Decision 0093: Include a recommended (non-normative) debugging workflow

**Status:** Accepted

**Context / Goal**
Even if workflow is not normative, documenting a recommended workflow improves
team alignment and makes it easier to debug ultra-large designs consistently.

**Decision**
- Option B: the spec includes a **recommended** (non-normative) debugging
  workflow section. It should describe how to use the standardized mechanisms
  effectively, without imposing hard requirements on viewer UX.

**Implications**
- The RFC/spec should maintain a clear separation: normative semantics vs
  non-normative workflow guidance.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0094: Trace is an on-demand, filtered debug artifact (offline file)

**Status:** Accepted

**Context / Goal**
Tracing everything by default is not practical for ultra-large designs. Trace
should be something you turn on only when debugging, and typically only for a
subset of suspicious modules/signals.

**Decision**
- Option A: linxtrace is primarily an **offline trace file** artifact.
- Trace emission is **off by default**; it is enabled on-demand for debugging.
- Trace must support **filtering** so users can limit emission to selected
  modules and signals.

**Implications**
- Writer/runtime must provide efficient filter configuration (at elaboration
  time and/or runtime) to avoid overhead when trace is off.
- Viewer workflow assumes partial traces are common.

**Source**
- User direction in #linx-core (2026-03-01): "a 默认不产生 只有debug时才会用… 也只是对部分怀疑的模块和信号开，要有过滤功能".

## Decision 0095: Filtering is primarily a writer/runtime responsibility

**Status:** Accepted

**Context / Goal**
To keep ultra-large design debugging practical, overhead must be avoided when
trace is off and when only a small subset of probes is of interest.

**Decision**
- Option A: filtering is primarily implemented in the **trace writer/runtime**.
  If a probe/scope is not enabled, it should **not emit** trace records.

**Implications**
- Filtering must be cheap to check at emission sites.
- Viewer-side filtering remains useful but is secondary.

**Source**
- User direction in #linx-core (2026-03-01): "a" + clarification: "probe在框架中默认不emit…".

## Decision 0096: Probe is a pyc primitive (an IR construct) lowered/emitted to C++

**Status:** Accepted

**Context / Goal**
We need a scalable way to instrument ultra-large designs without assuming all
signals are probed or that probes are purely a C++ runtime concept.

**Decision**
- A `probe` is a **pyc primitive** and thus a first-class construct in the **pyc
  IR**.
- Probes are selected/configured at the pyc/Python level and are **lowered /
  emitted into C++** as part of JIT/codegen.

**Implications**
- The IR must carry enough metadata (path/type/kind) for consistent trace
  emission.
- Filtering/enablement should be realized primarily during lowering/codegen so
  disabled probes do not impose runtime overhead.

**Source**
- User direction in #linx-core (2026-03-01): "probe是pyc的primitive，也是pyc的ir一种，然后emit到c++".

## Decision 0097: probe_id is assigned in IR/lowering (not in Python, not in C++ emit)

**Status:** Accepted

**Context / Goal**
Keep probe identity stable and consistent with the final canonical paths/types
that will actually be emitted, while avoiding coupling to C++ backend
implementation details.

**Decision**
- Option B: `probe_id` is assigned/generated during **IR lowering / codegen
  preparation** (after canonical paths are finalized), and is then emitted into
  C++.

**Implications**
- Python/front-end constructs do not need to guess final paths.
- C++ emission remains a mechanical lowering step.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0098: Probe registry/manifest is owned by IR/lowering; C++ keeps only lightweight mappings

**Status:** Accepted

**Context / Goal**
In a JIT architecture where probes are IR constructs, we want a single source of
truth for probe metadata (id/path/type/kind) that matches the final lowered
program and can be used to generate trace declarations/manifests.

**Decision**
- Option B: the primary probe registry (and any generated manifest) is produced
  and owned by **IR lowering / codegen preparation**.
- The C++ runtime/emitter may keep only the lightweight mappings needed for
  emission and trace writing.

**Implications**
- Aligns probe identity with Decision 0097 (id assigned in lowering).
- Keeps C++ runtime simpler and avoids duplicating control-plane logic.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0099: Support both self-describing traces and external manifests; prefer external manifests

**Status:** Accepted

**Context / Goal**
Traces are often partial (selective probes) and used for ultra-large designs.
Viewers benefit from fast access to probe metadata (id/path/type/kind) without
having to scan the entire trace payload.

**Decision**
- Option B: support both:
  - **Self-describing** traces (probe declarations inside the trace), and
  - **External manifest** (sidecar file or referenced metadata)
- Prefer external manifests as the primary workflow.

**Implications**
- The toolchain should define a stable manifest format and linking mechanism
  (e.g., trace header references manifest hash/path).

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0100: External manifest is generated during IR/lowering

**Status:** Accepted

**Context / Goal**
The manifest should reflect the final canonical probe metadata (id/path/type/kind)
used by emission, and should be available without running the full simulation.

**Decision**
- Option B: generate the external manifest during **IR lowering / codegen
  preparation**.

**Implications**
- Keeps manifest consistent with Decision 0097/0098.
- Enables tooling to prepare trace+manifest before execution.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0101: Filtering selection language uses hierarchical path patterns (glob/regex) + optional grouping

**Status:** Accepted

**Context / Goal**
Debugging ultra-large designs usually focuses on suspicious regions. Users need
an ergonomic way to select scopes/signals without enumerating all probe ids.

**Decision**
- Option B: filtering configuration supports hierarchical **path pattern**
  selection (glob/regex) and may optionally support tags/groups for convenience.

**Implications**
- Canonical path syntax must be stable enough for pattern matching.
- Tooling should provide helpers (autocomplete/search) to build these filters.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0102: Filtering is primarily compile-time (JIT-time) fixed

**Status:** Accepted

**Context / Goal**
Avoid runtime overhead in hot paths. Keep trace-off and trace-minimal modes as
cheap as possible.

**Decision**
- Option A: filtering selection is primarily applied at **JIT compile time**.
  Only enabled probes are lowered/emitted; others do not generate runtime emit
  code.

**Implications**
- Changing filters requires re-JIT/recompile (acceptable in debug workflow).
- Minimizes per-probe runtime checks.

**Source**
- User direction in #linx-core (2026-03-01): "a".

## Decision 0103: Event-driven simulation with memoized tick; port-level change detection

**Status:** Accepted

**Context / Goal**
Current LinxCore simulation scales poorly when the whole design is effectively
flattened and evaluated every cycle. We want module-instance SimObjects (Decision
0001) and an event-driven execution model so that only impacted modules re-run.

**Decision**
- Simulation is **event-driven**: a module instance re-runs `tick()` only when
  at least one of its **input ports changes**.
- Change detection granularity is **port-level** (not field/bit-level).
- Skipping `tick()` is defined as **pure memoization** (A2 semantics): if inputs
  are unchanged, outputs are guaranteed unchanged, so reusing previous outputs is
  semantically equivalent to recomputation (no implicit latching semantics).

**Implications**
- Port values should support a **version/epoch** concept to enable fast
  change-detection and fanout-based wakeups.
- Works naturally with SimObject-per-instance and hierarchical DFX/probe pathing.

**Source**
- User direction in #linx-core (2026-03-01): "a" for port-level change detection;
  follow-up: "a2" for memoization semantics.

## Decision 0104: Versioned value storage + fanout wakeup for event-driven scheduling

**Status:** Accepted

**Context / Goal**
To make event-driven simulation scalable, we need O(1) change detection and a
way to efficiently find downstream modules impacted by an output change.

**Decision**
- Port values are stored in versioned slots (e.g. a `ValuePool` where each value
  has an associated **version/epoch**).
- Each module instance caches last-seen input versions; it is scheduled when any
  input version differs.
- The runtime maintains a **fanout/dependency mapping** from an output port/value
  to the set of downstream module input ports that depend on it; when an output
  version increments, only affected modules are woken.

**Implications**
- Encourages a handle/index-based storage model compatible with large designs.
- Enables efficient dirty-queue scheduling without full-graph scans.

**Source**
- User direction in #linx-core (2026-03-01): "好" (agree) to record this as the
  next decision after 0103.

## Decision 0105: Value version increments only on semantic change (new != old)

**Status:** Accepted

**Context / Goal**
In an event-driven simulator, unnecessary wakeups kill the benefit. If a module
writes an output that is equal to the previous value, downstream modules should
not be rescheduled.

**Decision**
- Option B: bump a value/port's **version/epoch only when the new value differs
  from the old value**.

**Implications**
- Requires an equality/compare operation for port-level values.
- Avoids spurious fanout wakeups and improves steady-state performance.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0106: Two-level equality check for large port values (fast precheck + fallback)

**Status:** Accepted

**Context / Goal**
Port-level values may be large (struct/array/wide bits). Always doing a full
byte/word compare can dominate runtime and erase event-driven wins.

**Decision**
- Option B: use a **two-level** change check:
  1) a fast precheck (e.g. cached hash/signature, dirty flag, chunk summary)
  2) if needed, a full compare fallback to preserve exact semantics

**Implications**
- Keeps strict correctness while reducing average-case compare cost.
- Requires defining what metadata is stored alongside values (e.g. per-value
  signature).

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0107: Event-driven tick; batch transfer

**Status:** Accepted

**Context / Goal**
We want event-driven speedups without complicating the state-commit semantics.
The `tick()` phase benefits from selective evaluation; the `transfer()` phase is
more naturally expressed as a simple commit step.

**Decision**
- Option B: the dirty-queue/event-driven scheduler drives **`tick()`**.
- **`transfer()` remains batch-style** (e.g. run for all modules with state, or
  via a simple precomputed list), rather than being event-driven.

**Implications**
- Simplifies correctness reasoning: commit semantics do not depend on scheduler
  details.
- Keeps the door open for later optimization (e.g. limiting transfer to stateful
  modules) without changing the conceptual model.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0108: Initial dirty set via fanout from external inputs

**Status:** Accepted

**Context / Goal**
At simulation start/reset, we need an initial set of modules to evaluate.
Marking the whole design dirty negates the event-driven benefit.

**Decision**
- Option B: compute the initial dirty set by starting from **external/top-level
  input ports** and doing a single **fanout propagation** to mark downstream
  module instances dirty.

**Implications**
- Requires a representation of external inputs as versioned values/handles.
- Avoids full-graph evaluation at time 0 while still producing correct outputs.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0109: Within-cycle tick runs until the dirty queue is empty (converge)

**Status:** Accepted

**Context / Goal**
An event-driven scheduler can cascade changes through the graph. To preserve
cycle semantics equivalent to recomputing combinational effects from changed
inputs, we need a clear within-cycle convergence policy.

**Decision**
- Option B: during a cycle's `tick()` phase, repeatedly process the dirty queue
  **until it is empty** (i.e. run to a fixed point within the cycle).

**Implications**
- Requires a strategy for combinational cycles (e.g. detection, iteration cap,
  or explicit modeling rules) to avoid infinite oscillation.

**Source**
- User direction in #linx-core (2026-03-01): "b".

## Decision 0110: Combinational cycle handling: MLIR verification + runtime iteration cap (error)

**Status:** Accepted

**Context / Goal**
With Decision 0109 (run-to-empty within a cycle), combinational loops/oscillation
could cause non-termination or unstable behavior. We need a clear policy that is
scalable and fits the MLIR-based toolchain.

**Decision**
- Option B: treat combinational cycles as **invalid by default**.
- Add an **MLIR-level verification/check** to detect illegal combinational
  cycles in the relevant IR graph(s) and fail fast with a clear diagnostic.
- Add a **runtime safety net**: an iteration cap / oscillation detection during
  within-cycle convergence; on hit, stop and report an error (with enough path
  context to debug).

**Implications**
- Most cases should be caught statically in MLIR, keeping runtime overhead low.
- The runtime cap prevents hangs if a bad graph slips through or is constructed
  dynamically.

**Source**
- User direction in #linx-core (2026-03-01): "b" and reminder: "我们是mlir需要检查".

## Decision 0111: TemplateSpec hardening contract (Python @const/spec → MLIR + C++ metadata)

**Status:** Accepted

**Context / Goal**
pyCircuit's template/spec system intentionally lives at JIT elaboration time:
Python `pycircuit.spec` objects and `@const` metaprogramming are powerful for
constructing immutable compile-time structures (signatures, structs, collections
of module instances). However, after JIT, Python objects are gone and the C++
runtime/event-driven simulator must not depend on them.

**Decision**
- Treat all Python template/spec objects (`pycircuit.spec.*`, `@spec.valueclass`
  instances, and `@const`-returned containers) as **elaboration-time only**.
- Any information required by runtime semantics (event-driven scheduling,
  port/value layout, instance pathing/DFX/probes, fanout dependencies, etc.) MUST
  be **hardened at JIT-time** into one or both of:
  - **MLIR IR/attributes** (for verification + codegen inputs)
  - **C++ runtime metadata tables/blobs** (for simulation/DFX at runtime)
- The C++ runtime must rely only on hardened MLIR/codegen outputs + runtime state
  (ValuePool/StatePool/etc.), never on live Python objects.

**Implications**
- Strong separation of concerns:
  - Python: authoring + metaprogramming + deterministic elaboration
  - MLIR: structural truth + verification (incl. combinational cycle checks)
  - C++: fast event-driven execution using versioned values + fanout wakeups
- Requires defining stable serialized forms for key spec artifacts (e.g.
  signature/struct layouts, instance paths, connection graphs).

**Source**
- User request in #linx-core (2026-03-01): "请再读一下pyc的template spec设计。它可以做Python的数据结构，但是jit之后就没有了" and follow-up "好你先把契约写了".

## Decision 0112: MLIR dialect is the single semantic source; C++ sim and Verilog emission must be logically equivalent

**Status:** Accepted

**Context / Goal**
In the pyCircuit flow, MLIR serves as the "semantic truth" of the design. We
must support both:
- a fast C++ event-driven simulator backend, and
- an MLIR→Verilog emission backend.

To avoid divergence, the two backends must implement the *same* semantics.
After the C++ simulator passes, the Verilog emission should require no special
"fixups" and should also pass the same workloads.

**Decision (strong constraint)**
- Treat the pyc IR dialect (and its verified invariants) as the **single source
  of truth** for design semantics.
- The MLIR→C++ simulator backend and the MLIR→Verilog backend MUST be
  **logically equivalent** implementations of that same MLIR semantics.
- Backend-specific patches that change semantics are disallowed; semantic fixes
  must be made at the MLIR dialect semantics / verification / lowering rules so
  that both backends inherit the fix.

**Implications**
- MLIR verification is a gatekeeper: if it passes, both backends operate under
  the same validated assumptions (types, connections, illegal comb cycles, etc.).
- Regression should include **dual-backend equivalence tests**: identical
  stimulus, sampled at defined observation points (e.g. pre/post transfer), with
  C++ sim traces compared against Verilog-sim traces.

**Source**
- User direction in #linx-core (2026-03-01):
  - "verilog和cpp要等价" / "cpp仿真通了以后，verilog也不要改，也应该跑通"
  - "这就是pyc ir方言设计理念。要写成强约束"

## Decision 0113: Equivalence observation points are part of the semantics (pre/post transfer + defined sampling)

**Status:** Accepted

**Context / Goal**
Dual-backend equivalence testing (Decision 0112) only works if we precisely
define *when* values are observed within a cycle. Otherwise, trace comparisons
will produce false mismatches caused by sampling at different points rather than
real semantic divergence.

**Decision (strong constraint)**
- The dialect/runtime defines two canonical observation points per cycle:
  - **TICK-OBS (pre-transfer):** after within-cycle `tick()` convergence
    (Decision 0109) completes, but before `transfer()` commits state.
  - **XFER-OBS (post-transfer):** after `transfer()` batch commit
    (Decision 0107) completes.
- Probes/trace points MUST declare which observation point they sample at.
- C++ sim and Verilog sim equivalence comparisons MUST compare values sampled at
  the same named observation point.

**Implications**
- Clarifies semantics for reg/state visibility: state updates become visible at
  XFER-OBS; combinational effects of current inputs are visible at TICK-OBS.
- Makes it possible to generate consistent trace/probe code in both backends.

**Source**
- User direction in #linx-core (2026-03-01): "好继续" (continue; write the
  observation-point constraint as a decision).

## Decision 0114: Memory (mem/array) semantics are explicit and backend-stable (read timing + write commit + RDW rule)

**Status:** Accepted

**Context / Goal**
Memories are the most common source of "CPP sim passes, Verilog sim differs" if
read/write timing is implicit. To satisfy Decision 0112 (dual-backend logical
 equivalence) and Decision 0113 (observation points), memory behavior must be
part of the dialect semantics, not an implementation detail.

**Decision (strong constraint)**
- The dialect defines cycle semantics for memories in terms of the same
  observation points:
  - **Reads during `tick()` observe the pre-transfer memory state** (the state
    committed at the end of the previous cycle).
  - **Writes are committed during `transfer()`** and become visible at
    **XFER-OBS** of the current cycle (and subsequently at TICK-OBS of the next
    cycle).
- Read-during-write (same address in same cycle) behavior MUST be defined by the
  dialect. Default rule:
  - **old-data** (read returns the pre-transfer value; write takes effect at
    XFER-OBS).
  - If alternative behavior is needed (write-first/no-change), it must be an
    explicit, typed memory op/attribute so both backends match.

**Implications**
- C++ event-driven simulation can implement mem as versioned state committed in
  batch transfer, matching Verilog's clocked-update model.
- Verilog emission must lower the same rule (e.g. sequential write + defined RDW
  semantics), and testbenches must sample at TICK-OBS/XFER-OBS consistently.

**Source**
- User direction in #linx-core (2026-03-01): "好" (continue to lock down mem
  semantics as a strong constraint).

## Decision 0115: Reset/initialization semantics are explicit and identical across backends

**Status:** Accepted

**Context / Goal**
Initialization is another common source of divergence between C++ simulators and
Verilog (e.g. Verilog `initial` blocks, X-propagation, tool-specific init). To
satisfy Decision 0112, reset/initial behavior must be defined by the dialect and
lowered identically.

**Decision (strong constraint)**
- The dialect must make reset/init explicit:
  - State-bearing elements (regs, memories, stateful modules) have a defined
    **initial value** and/or an explicit **reset** behavior.
  - No backend may rely on implicit Verilog initialization defaults or simulator
    quirks.
- If a design uses reset, its sampling/visibility is defined relative to the
  same observation points (Decision 0113):
  - Reset effects that update state are applied at **transfer/commit** and are
    visible at **XFER-OBS** (and thereafter).
  - Pure combinational reset gating affects TICK-OBS like any other comb logic.

**Implications**
- C++ backend: initialize StatePool/MemPool from hardened init metadata; apply
  reset via the same transfer path used for normal state updates.
- Verilog backend: emit explicit reset logic (and/or explicit init values if
  synthesizable) consistent with the dialect; avoid `initial` unless the dialect
  explicitly models it.

**Source**
- User direction in #linx-core (2026-03-01): "好继续" (continue; lock down
  reset/initial equivalence as a strong constraint).

## Decision 0116: Dialect supports 4-valued logic (X) to match Verilog; X is preserved across both backends

**Status:** Accepted

**Context / Goal**
Verilog simulation is inherently 4-valued (0/1/X/Z). If the pyc dialect is to be
Verilog-friendly and satisfy Decision 0112 (C++ sim ↔ Verilog equivalence), the
core value model must be able to represent and propagate unknowns (X) rather
than silently collapsing to 2-valued logic.

**Decision (strong constraint)**
- The pyc dialect value model MUST support **X (unknown)** for signals/ports and
  state elements.
- The C++ event-driven simulator backend MUST implement X-aware operations and
  comparisons, preserving X semantics (do not coerce X to 0/1).
- The Verilog emission backend MUST preserve the same X semantics as represented
  in the dialect; equivalence testing compares X consistently.

**Implications**
- Value representation likely needs (value_bits, known_mask) or an equivalent
  encoding; equality and "new!=old" checks (Decision 0105/0106) must be defined
  for X-aware values (e.g. version bump when either value_bits differs under
  known bits or known_mask changes).
- Memory/reg reset/init rules (Decision 0115) must specify whether init produces
  known values or X, and how X propagates.

**Source**
- User direction in #linx-core (2026-03-01): "方言要适配好verilog，所以需要支持x".

## Decision 0117: X-aware equality/versioning contract (value_bits + known_mask; change detection + fast signature)

**Status:** Accepted

**Context / Goal**
Decision 0105/0106 require "bump version only on semantic change" and a
performance-friendly two-level compare. With Decision 0116 (support X), we must
make equality/change-detection precise and backend-stable; otherwise the C++
event-driven scheduler and Verilog simulation will diverge.

**Decision (strong constraint)**
- Represent each logic value as two bitvectors of equal width:
  - `value_bits`: the 0/1 payload
  - `known_mask`: 1 = known, 0 = unknown (X)
- Define semantic equality (`eq`) and change detection (`changed`) as:
  - `eq(a,b)` iff `a.value_bits == b.value_bits` AND `a.known_mask == b.known_mask`
  - `changed(a,b)` is the negation of `eq(a,b)`
- Version bump rule (refines Decision 0105): bump version iff `changed(old,new)`.
- Two-level compare (refines Decision 0106):
  - Maintain a per-value **signature** computed from both `value_bits` and
    `known_mask` (e.g. hash of the pair).
  - Fast precheck compares signatures; on mismatch do full compare on both
    bitvectors to avoid hash-collision false negatives.

**Implications**
- C++ backend: `ValuePool` slots must store both bitvectors (or an equivalent
  packed form), and all primitive ops must propagate `known_mask`.
- Verilog backend: lowering must preserve the same semantics; for example, a
  4-valued vector in Verilog corresponds to the same `(value_bits, known_mask)`
  model for equivalence testing.
- Probe/trace serialization must include X information (known_mask) to make
  dual-backend traces comparable.

**Source**
- User direction in #linx-core (2026-03-01): "好" (continue; lock down X-aware
  change detection and fast compare semantics).

## Decision 0118: Dialect supports Z (high-impedance) explicitly; tri-state/resolve semantics are defined

**Status:** Accepted

**Context / Goal**
To be Verilog-compatible, X support alone is not sufficient: Verilog also models
**Z (high-impedance)** and resolution on nets with multiple drivers. If the
pyc dialect is to emit Verilog that is logically equivalent to the C++ simulator
(Decision 0112), Z and multi-driver resolution must be explicit dialect
semantics, not left to backend interpretation.

**Decision (strong constraint)**
- The pyc dialect value model MUST support **Z** in addition to 0/1/X.
- The dialect MUST define where Z is legal:
  - Z may appear on *nets/ports/wires* that represent tri-state connectivity.
  - Z is illegal for *state elements* (regs/mems) unless explicitly modeled.
- The dialect MUST define multi-driver **net resolution** rules that both
  backends implement identically. Default rule set (Verilog-like):
  - If all active (non-Z) drivers agree on a known 0/1 → resolved = that value.
  - If no active drivers (all Z) → resolved = Z.
  - If conflicting active drivers or any unknown participation → resolved = X.
- The C++ simulator MUST implement resolution using the dialect rule, not by
  ad-hoc last-writer-wins.

**Implications**
- Requires distinguishing *net* vs *variable/state* in the dialect lowering and
  metadata (single-driver variables can bypass resolution for performance).
- Equivalence tests must sample resolved net values (post-resolution) at the
  defined observation points (Decision 0113).

**Source**
- User direction in #linx-core (2026-03-01): "需要" (need; add Z support).

## Decision 0119: Compile-time WNS/TNS-equivalent checks are logic-depth based (not timing); thresholds are compiler options

**Status:** Accepted

**Context / Goal**
We want early (compile-time) feedback on "too deep" combinational logic without
requiring a full timing model/library. In MLIR, we can estimate *logic depth*
(number of logic levels / longest-path depth) and use this as a WNS/TNS-like
proxy to gate designs during compilation.

**Decision (strong constraint)**
- The pyc dialect/toolchain MUST provide a compile-time check that computes
  **depth-based slack** for combinational paths, analogous to WNS/TNS:
  - Define per-endpoint **depth_arrival** as the maximum combinational depth
    from valid sources (e.g. regs/Q, primary inputs, memory read outputs) to the
    endpoint (e.g. regs/D, primary outputs, explicit timing endpoints).
  - Define a user-specified **depth_budget** (integer levels) provided as a
    compiler option/flag.
  - Define **depth_slack = depth_budget − depth_arrival**.
  - **WNS-equivalent** = minimum depth_slack across endpoints.
  - **TNS-equivalent** = sum of negative depth_slack across endpoints.
- The check MUST run in the MLIR pipeline and emit source-located diagnostics
  when thresholds are violated.
- The check MUST be backend-stable: C++ sim and Verilog emission consume the
  same verified IR and do not redefine depth semantics.

**Implications**
- Provides deterministic, library-free compile-time gating.
- Establishes a clear upgrade path: a future STA/timing-based analysis can reuse
  the same endpoint graph and report format, swapping "depth" for "delay".

**Source**
- User direction in #linx-core (2026-03-01): "mlir可以估计逻辑级数而不是timing…threshold是编译选项".

## Decision 0120: Value model upgrade plan: first-class 4-valued logic + explicit net/var split

**Status:** Accepted

**Context / Goal**
Current prototype uses plain MLIR integers (`iN`) as data values, which is
2-valued and cannot faithfully model Verilog's 4-valued semantics (Decision 0116
X + Decision 0118 Z). Z also implies multi-driver net resolution, which requires
an explicit notion of nets vs variables/state.

**Decision (required changes)**
- Introduce a first-class PYC value model that can represent **0/1/X/Z**.
- Make **net vs variable/state** explicit in IR and verifiable:
  - *net*: may have multiple drivers and uses the dialect-defined resolution rule
    (Decision 0118).
  - *var/state*: must be single-driven; no resolution; illegal to produce Z.
- Update all core comb ops to be defined over the 4-valued model (X/Z-aware),
  not implicitly over 2-valued integers.

**Implications**
- Enables backend-stable semantics for both C++ sim and Verilog emission.

**Source**
- Gap analysis request in #linx-core (2026-03-01): "请分析一下现有的pyc ir，还有哪些欠缺的" and follow-up: "把上述欠缺都写入decisions中".

## Decision 0121: Observation points are IR-visible and required for probes/trace and equivalence

**Status:** Accepted

**Context / Goal**
Decisions 0112/0113 define canonical observation points (TICK-OBS/XFER-OBS).
The current IR spec does not make these visible or enforceable, which risks
trace/equivalence drift across backends.

**Decision (required changes)**
- Extend IR/metadata so probes/trace sites can explicitly declare sampling at
  **TICK-OBS** or **XFER-OBS**.
- Define and document which ops take effect in tick vs transfer, so sampling is
  well-defined.

**Implications**
- Eliminates "false mismatches" in dual-backend comparisons.

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0122: Memory semantics must be normalized at the dialect level (tick-read vs transfer-write + RDW)

**Status:** Accepted

**Context / Goal**
The prototype primitive set includes both combinational-read and synchronous-read
memories, but the dialect-level semantics are not unified, risking divergence
between C++ sim and Verilog.

**Decision (required changes)**
- Lift memory behavior into explicit dialect semantics:
  - define read timing relative to TICK-OBS/XFER-OBS (Decision 0114)
  - define write commit at transfer
  - define RDW behavior (default old-data) as part of the op/attr contract
- Require MLIR verification to reject ambiguous/underspecified memory behavior.

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0123: Combinational cycle legality must be enforced by an MLIR verifier pass with instance-aware diagnostics

**Status:** Accepted

**Context / Goal**
Decision 0110 requires MLIR-level comb-cycle detection. The current IR spec
includes constructs that can express feedback (e.g. `pyc.wire/pyc.assign` and
cross-instance connections) but does not define the verification pass or graph
construction.

**Decision (required changes)**
- Implement an MLIR pass/verifier that builds the combinational dependency graph
  (including across `pyc.instance` boundaries) and rejects illegal cycles.
- Diagnostics must include enough hierarchical/instance-path context to debug.

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0124: Depth (WNS/TNS proxy) requires a specified counting model and endpoint rules

**Status:** Accepted

**Context / Goal**
Decision 0119 defines depth-based WNS/TNS equivalents, but the prototype does not
specify the counting model (which ops contribute depth) or the exact endpoint
set, which would make results unstable.

**Decision (required changes)**
- Specify depth counting rules in the dialect/toolchain:
  - which ops count as +1 logic level (e.g. add/mux/compare)
  - which ops are depth-neutral (e.g. alias/bitcast/wire plumbing)
  - how `pyc.comb` regions contribute (flattened by contained ops)
  - how instance boundaries contribute (depth propagates through instance I/O)
- Specify valid sources/endpoints for depth analysis.

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0125: TemplateSpec hardening must materialize required runtime/DFX metadata in MLIR (no Python dependency)

**Status:** Accepted

**Context / Goal**
Decision 0111 states Python TemplateSpec is elaboration-time only. The current
IR spec does not fully enumerate the hardened metadata required for runtime/DFX
(instance paths, port naming/layouts, probe maps, etc.).

**Decision (required changes)**
- Define a stable hardened metadata surface in MLIR for:
  - hierarchical instance paths/names
  - port/result names + signature/layout identity
  - probe/trace mapping to value slots
  - (if needed) fanout/connectivity metadata for schedulers/DFX

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0126: Multi-clock/CDC legality rules must be verified at MLIR level (no combinational cross-domain paths)

**Status:** Accepted

**Context / Goal**
The IR spec claims multi-clock modeling and strict ready/valid semantics, and the
primitive layer includes async FIFO / CDC synchronizers. However, cross-domain
legality constraints are not yet a first-class, verified dialect contract.

**Decision (required changes)**
- Make clock domains explicit where relevant (ops/values annotated or typed).
- Add MLIR verification rules/passes:
  - forbid combinational paths crossing clock domains
  - require CDC to occur only via explicit CDC primitives/ops
  - validate async FIFO and CDC primitive parameter constraints

**Source**
- Gap analysis request in #linx-core (2026-03-01).

## Decision 0127: Define the combinational dependency graph precisely (what is a node/edge; instance crossing; cut points)

**Status:** Accepted

**Context / Goal**
Comb-cycle legality (Decision 0110 / 0123) and depth/WNS proxy (Decision 0119 /
0124) require a single, deterministic definition of the "combinational
dependency graph". Without a precise graph model, different passes/backends will
compute different answers, and dual-backend equivalence will drift.

**Decision (strong constraint / required changes)**
- Define a single canonical **CombDepGraph** for the dialect/toolchain.
- Graph nodes represent *values at observation within tick* (i.e. combinational
  signals/nets after resolution where applicable).
- Graph edges represent *combinational dependence* of a node's value on another
  node's value within the same TICK phase.
- Instance crossing is part of the graph:
  - `pyc.instance` creates edges from caller operands → callee inputs, and from
    callee outputs → caller results.
  - Graph construction must be instance-aware and preserve hierarchical context
    for diagnostics.
- Define **cut points** (break combinational dependence) explicitly; at minimum:
  - `pyc.reg` state boundary (Q is a source; D is a sink)
  - memory state boundaries per the memory op semantics (Decision 0114/0122)
  - CDC/async FIFO boundaries (Decision 0126)

**Implications**
- All comb-cycle checks, depth checks, and scheduler fanout derivation operate on
  the same CombDepGraph definition.

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01): "先重新看一下4.0的架构变动…逻辑环还有问题的" and follow-up: "把这些都写在decision中".

## Decision 0128: Strong policy: combinational loops are illegal, including those involving net resolution (Z)

**Status:** Accepted

**Context / Goal**
Event-driven tick-to-fixpoint simulation (Decision 0109) plus Z/net resolution
(Decision 0118) can create subtle feedback loops. To keep semantics simple,
backend-stable, and Verilog-equivalent, we must forbid combinational loops in
all forms.

**Decision (strong constraint)**
- Any cycle in the CombDepGraph (Decision 0127) is **illegal by default**.
- This includes cycles that arise through:
  - explicit SSA/wire backedges (`pyc.wire/pyc.assign`)
  - cross-instance combinational paths
  - multi-driver net resolution / tri-state networks (Z)
- Designs requiring feedback must express it using explicit stateful elements
  (regs/mems/explicit sequential primitives) so the loop is cut by a transfer
  boundary.

**Implications**
- MLIR verifier must report cycles with a hierarchical path to the responsible
  ops/ports.
- Runtime iteration cap remains as a safety net, but well-formed IR must not
  rely on it.

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01).

## Decision 0129: 4-valued encoding is (value_bits, known_mask, z_mask); equality/version/signature cover all three

**Status:** Accepted

**Context / Goal**
Decision 0116/0118 require X and Z. Decision 0117 currently specifies an X-aware
2-vector model, but Z requires an explicit third component to keep C++ sim and
Verilog emission logically equivalent and to make event-driven change detection
sound.

**Decision (strong constraint / required changes)**
- Represent each logic value as three equal-width bitvectors:
  - `value_bits`: payload for known 0/1 bits
  - `known_mask`: 1 = known, 0 = unknown (X or Z)
  - `z_mask`: 1 = Z (high-impedance), 0 = not-Z
- Invariants:
  - `z_mask` implies unknown: for any bit, if `z_mask=1` then `known_mask=0`.
  - state variables/regs/mems must have `z_mask==0` unless explicitly modeled.
- Semantic equality/change/version/signature must be defined over the full
  triple:
  - `eq(a,b)` iff all three bitvectors match exactly
  - `changed(a,b)` = !eq(a,b)
  - version bumps iff changed
  - signatures/hashes include all three bitvectors, with full-compare fallback

**Implications**
- Makes event-driven wakeup decisions sound under Z/X.
- Provides a stable serialization for probe/trace and dual-backend comparison.

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01).

## Decision 0130: Net/var split is enforced: single-driver vars; resolved nets; resolve occurs during tick before TICK-OBS

**Status:** Accepted

**Context / Goal**
Supporting Z and multi-driver resolution requires distinguishing resolved nets
from single-driver variables/state. Without an explicit, verified split, C++ and
Verilog will diverge (e.g. last-writer-wins vs Verilog resolution).

**Decision (strong constraint / required changes)**
- The dialect defines two connectivity classes:
  - **var/state**: exactly one driver; no resolution; may not take Z.
  - **net**: may have multiple drivers; resolution is applied using Decision 0118.
- Add an MLIR verifier:
  - reject var/state values with 0 or >1 drivers
  - reject illegal Z production on var/state
- Define a canonical simulation order for nets:
  - net resolution is conceptually performed during `tick()` as part of the
    within-cycle convergence, producing resolved net values.
  - **TICK-OBS samples resolved nets** (resolution has happened).

**Implications**
- Makes both backends generate/interpret the same resolved value at sampling.

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01).

## Decision 0131: Depth/WNS proxy counting rules must account for net resolution and instance I/O

**Status:** Accepted

**Context / Goal**
Depth (Decision 0119/0124) must remain meaningful once nets and resolution are
introduced (Decision 0118/0130). Otherwise tri-state/resolution-heavy designs
will be mischaracterized.

**Decision (required changes)**
- Depth counting rules must explicitly include:
  - instance input→output propagation (depth flows through instance boundaries)
  - net resolution cost (resolution contributes a defined depth increment)
  - memory read semantics per memory kind

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01).

## Decision 0132: Hardened metadata must include the information needed to build CombDepGraph and diagnostics (paths, ports, probe maps)

**Status:** Accepted

**Context / Goal**
CombDepGraph-based verification/analysis and strong DFX require stable metadata
(Decision 0111/0125). The metadata must be sufficient not just for codegen, but
also for graph construction and hierarchical diagnostics.

**Decision (required changes)**
- Harden MLIR metadata for at least:
  - stable hierarchical instance identifiers/paths (with shortening rules from
    Decision 0002)
  - stable port/result naming and signature/layout identities
  - probe/trace maps that identify value/state slots and observation points
  - enough connectivity information to attribute graph edges back to IR/paths

**Source**
- Architecture re-evaluation request in #linx-core (2026-03-01).

## Decision 0133: Python control-flow is allowed, but must lower to static hardware; dynamic SCF is forbidden post-lowering

**Status:** Accepted

**Context / Goal**
pyc4.0 wants a Pythonic, serial-programming frontend with `if`/`for` and helper
functions, while keeping the backend IR as fully static hardware for scalable
verification, scheduling, and emission.

The current toolchain already follows this direction:
- `pyc-lower-scf-static` unrolls constant-bounded loops and lowers `scf.if` to
  mux networks.
- `pyc-check-no-dynamic` rejects residual `scf.*` and `index` values.

We make this a stable contract for pyc4.0.

**Decision (strong constraint / required changes)**
- The Python frontend MAY expose structured control flow (`if`/`for`) and helper
  functions as ergonomic authoring features.
- The backend MLIR pipeline MUST fully lower these constructs into static PYC
  hardware ops:
  - `scf.for` must be statically unrolled (bounds/step are compile-time
    constants; induction variable must not be used in hardware computations).
  - `scf.if` must lower to a mux network by speculating both branches and muxing
    yielded values (side-effect-free within branches), or be constant-folded.
- After lowering, dynamic control flow MUST NOT remain:
  - Any remaining `scf.*` ops or `index`-typed SSA values are compilation errors
    (`pyc-check-no-dynamic` is a required gate).

**Implications**
- "Loop" in Python source is an *authoring convenience* only; it does not imply
  a runtime loop in the generated hardware IR.
- This contract enables deterministic comb/depth analysis and backend-stable
  emission.

**Source**
- User question in #linx-core (2026-03-01): "但是我们有jit，展开成ssa pyc ir后还有loop吗？"

## Decision 0134: Comb-cycle legality must be instance-aware; InstanceOp is NOT a combinational cut point

**Status:** Accepted

**Context / Goal**
The existing prototype comb-cycle checker (`pyc-check-comb-cycles`) builds a
wire/assign dependency graph and currently treats `pyc.instance` as a
"sequential" definition, which cuts dependencies and can hide cross-instance
combinational cycles.

In pyc4.0, module boundaries are preserved as SimObjects (Decision 0001), but
combinational semantics must remain combinational across module boundaries.

**Decision (strong constraint / required changes)**
- `pyc.instance` MUST NOT be treated as a combinational cut point.
- CombDepGraph construction (Decision 0127) MUST traverse instance boundaries:
  - caller operand → callee input edges
  - callee output → caller result edges
- Only explicitly stateful primitives (reg/mem/fifo/async_fifo/cdc_sync/etc.)
  may act as combinational cut points.
- The comb-cycle verifier MUST report cycles with hierarchical context:
  - instance path segments (shortened per Decision 0002)
  - port/result names where the dependence crosses an instance boundary

**Implications**
- Fixes "logic loop still exists" cases that occur across module boundaries.
- Ensures the IR-level legality contract matches both C++ sim and Verilog
  semantics.

**Source**
- Architecture re-evaluation in #linx-core (2026-03-01): "逻辑环还有问题的" + inspection of current pass behavior.

## Decision 0135: Logic-depth (WNS/TNS proxy) analysis must propagate through instance boundaries

**Status:** Accepted

**Context / Goal**
The current `pyc-check-logic-depth` prototype treats `pyc.instance` as a
sequential op (depth cut), which underestimates depth in hierarchical designs.
This breaks the intent of depth-based compile-time gating (Decision 0119).

**Decision (strong constraint / required changes)**
- Depth analysis is defined over the same instance-aware CombDepGraph (Decision
  0127/0134).
- `pyc.instance` does not reset depth; depth contributions of a callee propagate
  to the caller outputs.
- The endpoint set for depth/WNS/TNS must include:
  - module return values (primary outputs)
  - `pyc.assert` conditions
  - sequential-op inputs that form next-state/commit boundaries (reg D, mem
    write inputs, fifo control/data inputs)

**Implications**
- Depth gates become stable and meaningful on large hierarchical designs.

**Source**
- User direction in #linx-core (2026-03-01): compile-time depth thresholds + current pass inspection.

## Decision 0136: Type inference belongs in the frontend; MLIR backend enforces a flat emission contract via gates

**Status:** Accepted

**Context / Goal**
The existing flow relies on frontend inference (literal widths, implicit
zero/sign-extension for convenience) and a backend gate (`pyc-check-flat-types`)
that enforces that only emission-safe types remain.

In pyc4.0 we will likely introduce richer *authoring* types (bundles/structs and
4-valued logic), but the emission backends still benefit from a flat, uniform IR
surface.

**Decision (strong constraint / required changes)**
- Frontend responsibilities:
  - infer widths/sign where ergonomic (e.g. literal width defaults)
  - track structured interfaces (bundle/struct) and lower them deterministically
    to flat wires before emission
  - attach hardened metadata needed for layout/field mapping (Decision 0125/0132)
- Backend responsibilities:
  - run required gates that reject unsupported residual types or dynamics:
    - `pyc-check-no-dynamic`: no `scf.*`/`index`
    - `pyc-check-flat-types`: only allowed lowered emission types
- Any new "authoring types" introduced for pyc4.0 MUST have a lowering plan to
  the flat emission surface.

**Implications**
- Keeps `pycc` emitters simple and stable, while allowing a Pythonic frontend.

**Source**
- User note in #linx-core (2026-03-01): "很多类型是可以推导出来的".

## Decision 0137: Frontend must provide explicit constructs for nets/multi-driver intent; do not rely on wire/assign hacks

**Status:** Accepted

**Context / Goal**
The prototype uses `pyc.wire/pyc.assign` both for SSA backedges and as an
implicit mechanism that could be (ab)used to encode multi-driver connectivity.
With pyc4.0 adding Z/resolution and strict comb-cycle legality, we need explicit
frontend constructs so intent is unambiguous and verifiable.

**Decision (required changes)**
- Provide explicit frontend APIs/IR forms to express:
  - single-driver variables/state (var)
  - multi-driver resolved nets (net)
  - tri-state drive (drive with enable producing Z when disabled)
- `pyc.wire/pyc.assign` should remain a backedge/SSA plumbing mechanism, but
  MUST NOT be the primary user-facing way to express multi-driver resolution.

**Implications**
- Makes net resolution semantics verifiable (Decision 0130) and avoids accidental
  comb loops due to ambiguous modeling.

**Source**
- User direction in #linx-core (2026-03-01): "好，把改进意见写成decision，尽可能详细".

## Decision 0138: Interface-first authoring: first-class Bundle/Struct types with deterministic flattening + one-shot probe expansion

**Status:** Accepted

**Context / Goal**
Large designs (e.g. LinxCore) are dominated by structured stage interfaces.
Authoring must be Pythonic and low-boilerplate: users should define interfaces
once and reuse them across modules, while the backend still emits flat wires.
DFX should be easy: probing a bundle should automatically expand into stable,
field-named probes.

**Decision (strong constraint / required changes)**
- Provide first-class authoring-time aggregate types (Bundle/Struct) with:
  - stable field order (schema-defined)
  - per-field metadata (width, signed intent, logic-kind)
  - deterministic flattening to the flat emission surface (Decision 0136)
- Add a frontend API to "probe a bundle" in one call:
  - bundle probe expands into per-field probes with stable names
  - probe expansion supports hierarchical prefixes and stage/lane naming helpers

**Implications**
- Reduces code volume drastically for large pipelines.
- Makes DFX naming stable and script-friendly.

**Source**
- User goal in #linx-core (2026-03-01): "pyc需要变得更加pythonic…probe，trace各种dfx能力在前端的扩展".

## Decision 0139: Const/template system must support canonicalizable const data structures + deterministic caching keys

**Status:** Accepted

**Context / Goal**
pyc4.0 needs a compile-time metaprogramming layer similar to C++ templates:
const data structures drive specialization and generate large hardware graphs.
All const computation must finish during JIT, and results must be usable as
stable cache keys for incremental builds.

**Decision (strong constraint / required changes)**
- Define a canonical const value domain that is fully serializable and stable:
  - primitives: bool/int/str/None
  - containers: tuple/list/dict with deterministic key ordering
  - frozen dataclasses / user objects via an explicit "template value" hook
- All `@const` evaluation MUST be complete during JIT and MUST NOT emit hardware
  IR or mutate module interfaces.
- Provide deterministic canonicalization of const values used in specialization:
  - identical semantic const values must produce identical cache keys
  - diagnostics must pinpoint the non-canonical/non-serializable value site

**Implications**
- Enables fast incremental rebuilds for huge designs.

**Source**
- User goal in #linx-core (2026-03-01): "它需要有const数据结构，就像c++ template一样。const也可以计算，但是都是在jit时间内算完".

## Decision 0140: DFX is a first-class frontend extension point: probes include observation point + tags; trace is configurable by instance globs

**Status:** Accepted

**Context / Goal**
Ultra-large designs require scalable DFX: probes and traces must be easy to add,
selectively enable, and correlate with hierarchy. pyc4.0 also requires
observation-point stability (Decision 0121) to keep C++/Verilog traces
comparable.

**Decision (strong constraint / required changes)**
- Frontend probe API MUST support:
  - explicit sampling point: `at = tick|xfer` (mapped to TICK-OBS/XFER-OBS)
  - optional tags (stage/lane/family) to drive downstream tools (pipeview, occ)
  - bundle expansion (Decision 0138)
- Trace configuration MUST support:
  - enable/disable by hierarchical instance glob
  - enable/disable by probe tags/families
  - bounded sampling windows and trigger conditions (for cosim/debug loops)
- Harden probe/trace maps into MLIR metadata (Decision 0132).

**Implications**
- Large designs can be debugged without hand-wiring `dbg__*` ports everywhere.

**Source**
- User goal in #linx-core (2026-03-01): "trace各种dfx能力在前端的扩展…debug也容易".

## Decision 0141: Incremental build is a core requirement: multi-layer caches + stable naming/layout must minimize rebuild scope

**Status:** Accepted

**Context / Goal**
To ramp up quickly on large designs, we need incremental builds: changes should
recompile only affected modules/passes and reuse cached artifacts whenever
possible.

**Decision (strong constraint / required changes)**
- Define a project build graph with deterministic cache keys:
  - Python/JIT stage: (source hash, const params key, dependency hashes)
  - MLIR stage: (input MLIR hash, pass-pipeline hash, compile options)
  - C++ stage: object cache keyed by (generated source hash, toolchain flags)
- Enforce stable naming and stable layout flattening so non-semantic changes do
  not invalidate caches (Decision 0125/0132/0138).
- The build system MUST support "compile only the touched modules" and avoid
  relinking/regenerating the world unless needed.

**Implications**
- Makes iteration on LinxCore-scale designs practical.

**Source**
- User goal in #linx-core (2026-03-01): "He needs incremental build…快速ramp up起来".

## Decision 0142: Cosim integration is a first-class workflow: standardize DUT commit/retire bundle + protocol schema versioning + DFX dump on mismatch

**Status:** Superseded by Decision 0158

**Context / Goal**
LinxCore already uses a QEMU lockstep cosim protocol (M1) based on commit traces
and sparse memory snapshots. pyc4.0 should generalize this into a standard
workflow so other designs can reuse it, and so mismatch diagnosis becomes faster.

**Decision (strong constraint / required changes)**
- Standardize a DUT-facing "commit/retire bundle" schema in the pyc ecosystem:
  - minimum fields: pc/insn/len/next_pc + wb + mem + trap groups (as in M1)
  - allow extension fields (uop_uid/block_bid/etc.) without breaking tools
- Cosim protocol MUST include schema identification/versioning:
  - `start` message carries `commit_schema_id` (or equivalent)
  - runner validates schema compatibility and fails early with a clear message
- On mismatch, tooling MUST support automatic DFX dump:
  - dump a configured set of probes (by tags/globs) around the mismatch window
  - include instance-path context and observation points

**Implications**
- Reduces time-to-root-cause by turning mismatches into rich, actionable
  diagnostics.

**Source**
- User goal in #linx-core (2026-03-01): "能和qemu进行cosim…probe，trace各种dfx能力…debug也容易" + existing LinxCore lockstep protocol.

## Decision 0143: Bundle/Struct flattening rules are deterministic and tool-visible (layout ID, field paths, packing)

**Status:** Accepted

**Context / Goal**
Decision 0138 introduces Bundle/Struct authoring types, but incremental build,
DFX, and backend emission require deterministic, standardized flattening:
- two identical schemas must flatten identically across runs
- tooling must be able to map (bundle_field_path) ↔ (flat wire slice)

**Decision (strong constraint / required changes)**
- Define canonical flattening rules:
  - field order is schema order (no hash-map iteration)
  - packing is MSB-first within each field; bundle concatenation order is
    schema order (documented)
  - no implicit padding unless explicitly requested by schema
- Every flattened aggregate MUST carry:
  - `layout_id`: stable hash of the schema (field names + widths + logic-kind)
  - a field map: (field_path → [lsb,width]) suitable for probes and emitters
- Emitters MUST preserve field-path names for DFX (Decision 0140) and must not
  reorder fields.

**Implications**
- Enables stable cache keys, stable probe names, and deterministic codegen.

**Source**
- Follow-up in #linx-core (2026-03-01): "好继续补" after Bundle/probe decisions.

## Decision 0144: Const canonicalization algorithm is standardized; failures produce pinpoint diagnostics

**Status:** Accepted

**Context / Goal**
Const/template data drives specialization and incremental caching (Decision
0139/0141). Without a standardized canonicalization algorithm, semantically
identical const values may miss caches, and debugging const failures becomes
painful.

**Decision (strong constraint / required changes)**
- Define a single canonicalization algorithm for const values:
  - dict keys are sorted by their canonical string form
  - tuples/lists are canonicalized element-wise
  - frozen dataclasses/user objects canonicalize via an explicit template hook
  - forbid non-deterministic values (e.g. object ids, file handles, lambdas)
- Canonicalization failures MUST produce diagnostics that include:
  - python source location (file:line)
  - const path (e.g. `params.cfg.table[3].opcode`)
  - the rejected value type and guidance to fix

**Implications**
- Makes const metaprogramming scale to very large generators.

**Source**
- Follow-up in #linx-core (2026-03-01): "好继续补" after const/template decisions.

## Decision 0145: Trace configuration DSL: instance globs + tags + triggers + sampling windows

**Status:** Accepted

**Context / Goal**
Decision 0140 requires configurable tracing. Large designs need a concise DSL to
enable trace/probes without editing design code, and to capture short windows
around triggers (especially for cosim mismatches).

**Decision (strong constraint / required changes)**
- Define a trace configuration DSL (file or CLI flag) that supports:
  - hierarchical instance glob patterns (with the same shortening rules as
    Decision 0002)
  - probe tags/families selection (pv/occ/custom)
  - triggers (first commit at PC==X, mismatch event, user predicate)
  - sampling windows (N cycles before/after trigger, max bytes/events)
- Trace config MUST be consumable by both:
  - C++ runtime tracing
  - Verilog/Verilator tracing

**Implications**
- Enables "turn on just enough trace" workflows for fast bug isolation.

**Source**
- Follow-up in #linx-core (2026-03-01): "好继续补" after DFX decisions.

## Decision 0146: Cosim commit bundle compatibility rules: unknown fields are ignored; groups obey validity gating; schema evolution is non-breaking

**Status:** Superseded by Decision 0158

**Context / Goal**
Cosim commit/retire bundles (Decision 0142) will evolve. Tools must remain
compatible across versions while still comparing the architectural essentials.

**Decision (strong constraint / required changes)**
- Commit bundle fields are grouped with explicit validity gating:
  - `wb_*` fields only compared if `wb_valid==1`
  - `mem_*` fields only compared if `mem_valid==1`
  - `trap_*` fields only compared if `trap_valid==1`
- Unknown/extra fields in a commit record MUST be ignored by older runners
  (forward-compatible parsing).
- Schema evolution rules:
  - adding optional fields is non-breaking
  - changing meaning/units of an existing field requires a new schema id

**Implications**
- Prevents toolchain lockstep breakage when adding DFX-friendly fields.

**Source**
- LinxCore M1 protocol normalization rules + follow-up in #linx-core (2026-03-01).

## Decision 0147: Generated C++/MLIR artifacts must be stable under non-semantic edits to maximize incremental build hit rate

**Status:** Accepted

**Context / Goal**
Incremental build (Decision 0141) depends on stable artifacts. Non-semantic
source edits (formatting, variable renames) should not cause widespread rebuilds
or invalidate probe paths.

**Decision (strong constraint / required changes)**
- Enforce stable naming policies where possible:
  - module symbol names derive from (base_name + params_hash) deterministically
  - temporary SSA names are not part of cache keys
  - debug/probe names come from explicit user names and schema paths, not from
    transient compiler-generated identifiers
- Layout IDs (Decision 0143) and schema IDs (Decision 0146) are the stable
  anchors for caching and tool compatibility.

**Implications**
- Keeps iteration speed acceptable on LinxCore-scale projects.

**Source**
- Follow-up in #linx-core (2026-03-01): "好继续补" after incremental build decisions.

## Decision 0148: CycleAwareSignal is the primary pyCircuit 6 authoring model

**Status:** Accepted

**Supersedes:** Decision 0010

**Context / Goal**
pyCircuit 6 needs one product model for pipeline timing. The existing
`CycleAwareSignal`, `CycleAwareDomain`, `CycleAwareCircuit`, and
`compile_cycle_aware()` implementation already carries logical-cycle provenance
and performs automatic cycle balancing. Treating that surface as legacy creates
an artificial split between the implementation, examples, and product docs.

**Decision (strong constraint)**
- `CycleAwareSignal` is the canonical scalar design signal in pyCircuit 6.
- `CycleAwareDomain` owns the authoring-time logical occurrence index. Its
  `next()`, `prev()`, `push()`, `pop()`, and `call()` operations may propagate
  cycle provenance across function and module composition.
- Cycle-aware provenance may cross module boundaries through hardened compile
  metadata. It is not restricted to a module-local sub-DSL.
- The compiler automatically inserts explicit `pyc.reg` delay chains when
  operands from different logical cycles must be aligned.
- `domain.signal()` plus `<<=` or `.assign()` is the canonical inferred-state
  form. Direct access to a signal's underlying wire is not a public design API;
  `wire_of()` is restricted to explicit I/O boundaries.
- `compile_cycle_aware()` remains a supported pyCircuit 6 compile entrypoint.
  The `@module`/`compile()` structural surface remains supported for explicit
  hierarchy and library construction, but it must lower to the same verified
  `pyc` MLIR semantics and must not define a competing timing model.
- The public V6 specification, tutorial, and architecture documents are the
  current product documentation. V5 documents and language labels are obsolete.

**Compatibility and verification**
- This is a hard-break documentation and governance transition. Do not add a
  compatibility mode that reenacts Decision 0010.
- MLIR remains the semantic source of truth. Automatic cycle balancing must be
  visible as ordinary stateful IR before backend emission.
- C++ and Verilog backends must consume the same balanced, verified IR.
- Changes to cycle inference or balancing require focused frontend tests, MLIR
  legality checks, the examples lane, both simulation lanes, and semantic
  regression evidence.

**Source**
- User direction (2026-08-31): unify the repository on pyCircuit 6, use the V6
  documents as current, supersede the earlier decision, and retain the
  CycleAwareSignal design.

## Decision 0149: Active runtime, trace, and semantic-gate names use pyCircuit 6

**Status:** Accepted

**Context / Goal**
After the language and CycleAwareSignal contract moved to pyCircuit 6, active
runtime and verification identifiers still exposed pyc4/v40 names. Those names
made current artifacts appear historical and created ambiguity in package and
gate documentation.

**Decision**
- The precompiled runtime library is `libpyc6_runtime`.
- The active `.pyctrace` schema magic is `PYC6TRC3`.
- The active semantic closure lane is
  `flows/scripts/run_semantic_regressions_v6.sh`.
- Generated manifests, CMake projects, CLI diagnostics, tests, CI, and product
  documentation must use those V6 identifiers.
- Existing files under `docs/gates/logs/` remain immutable historical evidence
  and may retain earlier identifiers.

**Compatibility and verification**
- Trace readers may retain explicit compatibility support for older schemas,
  but writers emit the V6 schema.
- Runtime and trace naming changes require packaging, generated-CMake, CLI,
  unit, and system-smoke coverage.

**Source**
- pyCircuit 6 repository convergence (2026-08-31).

## Decision 0150: Agentic Circuit remains a distinct upper-level IR and frontend inside the pyCircuit repository

**Status:** Accepted

**Context / Goal**
PTO-ISA is consolidating Agentic Circuit into `PTO-ISA/pyCircuit` so one
repository owns the complete architecture-to-hardware flow. Consolidation must
not collapse architecture/process semantics into the PYC hardware dialect or
create a second timing model that competes with pyCircuit 6.

**Decision (strong constraint)**
- ACIR remains an independent, upper-level MLIR dialect. Its architecture,
  process, and queue operations must not be folded into the `pyc` dialect.
- The `agentic_circuit` and `pycircuit` Python distributions and import
  namespaces remain distinct public surfaces in the shared repository.
- The Agentic Circuit frontend continues to emit ACPy and ACIR. Existing ACPy
  contract epochs and schemas may change only through an explicit contract
  decision and matching compatibility evidence.
- ACSim and gfsim remain the architecture-simulation lowering and runtime path.
  They are not aliases for, or implementations inside, `libpyc6_runtime`.
- Synthesizable ACIR lowers into verified PYC IR. ACIR-to-PYC integration must
  adapt to pyCircuit 6, `libpyc6_runtime`, and the CycleAwareSignal contract in
  Decision 0148; it must not reintroduce prior-version runtime names or bypass
  PYC verifiers.
- ACIR-to-PYC and the native pyCircuit frontend share the same verified PYC
  semantics and downstream C++ and Verilog backends.
- First-party Agentic Circuit and pyCircuit code in the consolidated repository
  is licensed under BSD-3-Clause. The owner-direction record and exact imported
  Git objects are maintained in
  `docs/legal/AC-RELICENSE-BSD-3-CLAUSE.md`.

**Migration and retirement rules**
- Preserve the Agentic Circuit `main` history and migrate open PR work with
  original commit and author provenance. Each old PR must receive an explicit
  migrated, superseded, or rejected disposition; no unique reviewed work is
  silently discarded.
- The original Agentic Circuit repository remains active until both the AC
  lanes and the existing PYC lanes pass from the consolidated checkout.
- Retirement requires at least ACIR/ACSim parser and verifier coverage,
  ACIR-to-gfsim execution, ACIR-to-PYC-to-C++/Verilog coverage, and the normal
  pyCircuit 6 frontend, simulation, and semantic-regression gates.
- After those gates pass, disable release, package, and CI authority in the old
  repository, make it private, and retain only `zhoubot` as a direct repository
  collaborator. PTO-ISA organization owners may retain access inherent to
  GitHub organization administration; the repository policy must not claim it
  can remove that inherited authority.
- The old repository is provenance only after retirement. New source changes,
  issues, releases, and packages belong in `PTO-ISA/pyCircuit`.

**Compatibility and verification**
- Keeping two Python namespaces does not authorize duplicate semantic
  implementations: both hardware paths converge on verified PYC IR.
- C++20 may remain target-local to Agentic Circuit targets during integration;
  repository-wide compiler-standard changes require a separate decision.
- Decision 0150 is implemented when the imported source, namespace boundaries,
  repo-local pyc6 lowering, build/install contracts, and AC/PYC closure evidence
  are present in `PTO-ISA/pyCircuit`.
- Changing the old repository's visibility, credentials, and archive state is
  a separate operational cutover. A blocked cutover does not make the merged
  compiler implementation unverified; it requires the old repository to remain
  active until the operational gate is resolved.

**Source**
- Repository-owner direction (2026-08-31): consolidate Agentic Circuit into
  pyCircuit; retain ACIR and its frontend; use BSD-3-Clause; preserve both
  Python namespaces; migrate existing PRs; test AC and PYC before retiring the
  old private repository.

## Decision 0151: Agentic Circuit epoch 0.4 introduces the provisional state Table

**Status:** Accepted

**Context / Goal**
Architecture models need a small, typed state array that can be observed and
updated without disguising it as a request/response memory service. The former
`ac.table(value, address=..., ...)` spelling was a wrapper over
`ac.memory.instance` and `ac.memory.request`; it did not provide Table state
semantics.

**Decision (strong constraint)**
- Agentic Circuit uses contract epoch `0.4`. Producers emit only `0.4`, and
  active consumers reject `0.3` and every other epoch. No compatibility mode is
  retained.
- `ac.table[entries, Entry](init=0)` declares a one-dimensional Table. `Entry`
  is a boolean, a fixed-width integer, or a flat struct containing only those
  scalar fields. Only an all-zero initial image is supported.
- `Table.view(index)` produces an elaboration-only `EntryView`. A view may be a
  lexical alias but may not be stored in a Queue, Table, struct, or across a
  cycle.
- `EntryView.read(...)` and Queue-driven `Table.view(selector).read(...)`
  always produce `Queue<Entry>`. A false Queue-driven `when` neither consumes
  the request nor produces output. A continuously true state-driven `when` may
  capture one Entry each tick when its output has capacity.
- Queue-driven `write` and `patch` consume a disabled update without proposing
  state. Each Table has at most one write-or-patch endpoint. `patch` is frontend
  sugar and must disappear before Frozen ACIR as `ac.table.get`, ordinary
  immutable field updates, and `ac.table.write`.
- Reads observe the old committed Entry when a write is proposed in the same
  tick. The proposal becomes visible at tick commit. An Entry already captured
  in an output Queue remains stable under backpressure.
- Static out-of-range indices are verifier errors. Dynamic out-of-range reads
  and writes report the stable runtime diagnostic
  `table_index_out_of_range`.
- Frozen ACIR defines `ac.table`, `ac.table.get`, `ac.table.read`,
  `ac.table.write`, and `ac.table.yield`. Table identity, owner visibility,
  endpoint regions, Entry types, endpoint completeness, and the single-writer
  rule are verifier obligations.
- The epoch `0.4` prototype is implemented by QueueGraph and typed gfsim C++.
  PYC lowering must stop at a stable `unsupported provisional Table`
  diagnostic. This explicit provisional boundary does not authorize partial
  semantics in a future PYC implementation.
- The removed request/response behavior remains available as `ac.memory`.
  Legacy `ac.table(...)` calls diagnose that migration instead of silently
  changing meaning.

**Deferred work**
- PYC/RTL lowering and cross-backend equivalence;
- `ac.firing` integration and atomic commit across Queue, Table, and Reg;
- state-driven writes, multiple writers, arbitration, and mutual-exclusion
  proof;
- multidimensional shapes, non-zero images, match/select, masked patches, and
  SRAM inference.

**Verification**
- Parser/printer/verifier coverage includes scalar and flat-struct Entries,
  static bounds, region types, visibility, endpoint completeness, and duplicate
  writers.
- gfsim coverage includes zero reset, disabled-write consumption, old-data
  reads, next-tick visibility, repeated state reads, and output backpressure.
- Frontend and generated-C++ coverage proves the
  `ACPy -> Frozen ACIR -> QueueGraph -> typed gfsim C++` vertical path and the
  stable PYC rejection boundary.

**Source**
- Stateful Table prototype direction (2026-09-01).

## Decision 0152: epoch 0.4 Table adds committed slots, match/choose, and state-driven updates

**Status:** Accepted

**Context / Goal**
The initial Decision 0151 Table can store a scoreboard, but an issue-style
model still cannot retain a backpressured request, scan committed entries,
choose one candidate, and update state without consuming a second Queue token.
This decision extends the provisional epoch `0.4` surface without changing the
global contract epoch or claiming a PYC/RTL realization.

**Decision (strong constraint)**
- `EntryView.write(value=..., enable=...)` and `EntryView.patch(enable=...,
  field=...)` accept a zero-Queue state-driven form. A false enable evaluates
  neither the dynamic index nor the value and creates no proposal. A true
  enable reads old committed state and commits at the tick edge. The existing
  Queue-driven form continues to consume disabled requests.
- `Table.match(predicate)` returns a frontend-only `CandidateSet`. Bit `i`
  corresponds to Entry `i`. The predicate is pure combinational logic over the
  committed Entry and immutable committed-state captures.
- `Table.choose(candidates, count=1, policy=...)` supports `first`, `min`, and
  `max`. `first` and equal keys select the lowest index. No candidate returns
  `valid=false,index=0`. `min/max` keys are unsigned fixed-width integers.
  Match/choose domains contain 1 through 64 entries; a larger Table remains
  legal but cannot use these operations in this prototype.
- CandidateSet and Selection values are elaboration-only, belong to one Table,
  and cannot be stored, Queue-carried, or retained across ticks. A public Table
  still has at most one write-or-patch endpoint.
- `ac.slot(queue)` owns one committed valid bit and one typed payload register.
  While empty it captures one Queue token; while full it backpressures the
  input. `valid` and `value` are read-only. The unique `release(when=...)`
  endpoint clears only valid and retains payload. A full slot that releases
  does not refill in the same tick.
- Frozen ACIR adds `ac.table.match`, `ac.table.match.yield`,
  `ac.table.choose`, `ac.table.choose.yield`, `ac.slot`, `ac.slot.get`,
  `ac.slot.release`, and `ac.slot.yield`. Verifiers own domain, type,
  visibility, purity, key-policy, result-width, unique-release, and
  single-writer legality.
- QueueGraph, the Python direct generator, the native QueueGraph C++ generator,
  and typed gfsim use the same old-state/commit, reset-zero, payload-retention,
  no-refill, and deterministic selection rules.
- Every Table/slot provisional operation remains outside PYC. The stable
  boundary is `unsupported provisional Table`; partial RTL lowering is
  forbidden.

**Deferred work**
- `ac.firing` and atomic transactions across Queue, Table, slot, and Reg;
- multidimensional Table, `count>1`, signed keys, round-robin selection,
  CandidateSet algebra, masked patches, multiple writers, non-zero images,
  nested Entries, response generation, and same-cycle slot refill;
- PYC/RTL lowering and cross-backend equivalence.

**Verification**
- Frontend and ACIR verifier tests cover state-driven writes, match/choose
  domains and policies, Table ownership, single writer, and unique release.
- gfsim tests cover disabled state updates, old-state reads, repeated enable,
  slot reset/capture/backpressure/release/payload retention/no-refill, and
  deterministic first/min/max selection.
- The state example is frozen to ACIR, planned through QueueGraph, generated by
  both C++ paths, compiled, and executed. PYC continues to reject it at the
  provisional boundary.

**Source**
- Agentic Circuit Table prototype extension direction (2026-09-02).

## Decision 0153: epoch 0.4 Table supports uniform masked state updates

**Status:** Accepted

**Context / Goal**
Issue tables and scoreboards commonly update every committed Entry selected by
one `match` result. Repeating scalar endpoints cannot express that operation
under the single-writer contract, and assigning source-order priority would
make simultaneous updates ambiguous. The prototype therefore needs one
explicit, atomic masked endpoint while retaining the existing scalar-index
surface.

**Decision (strong constraint)**
- `Table.view(candidates)` accepts a `CandidateSet` produced by `match` on the
  same Table. The masked view is elaboration-only and uses the existing
  1-through-64-entry match domain. Integer indices continue to produce the
  existing scalar `EntryView` with unchanged read/write/patch semantics.
- `MaskedEntryView.write(value=..., enable=...)` is state-driven and writes the
  same complete Entry value to every selected index. The value is a uniform
  committed-state expression; a per-Entry write lambda is not supported.
- `MaskedEntryView.patch(enable=..., field=...)` applies the same set of field
  assignments to every selected Entry. Each field may be a uniform expression
  or a pure `lambda entry: ...` evaluated from that selected Entry's old
  committed value. As with scalar patch, patch remains frontend sugar.
- A false enable evaluates neither mask nor value and creates no proposal. A
  true enable takes one old-state snapshot, computes all selected values, and
  commits them together at the tick edge. An empty mask is a successful no-op.
  Reads in that tick observe the old image.
- A masked endpoint is one logical writer. It cannot coexist with another
  scalar or masked write/patch endpoint on the same public Table. This decision
  does not add source-order priority, arbitration, or mutual-exclusion proof.
- Frozen ACIR adds `ac.table.masked_write`. Its mask is `!ac.var<iN>`, where
  `N` equals the Table entry count, and must be the same-Table result of
  `ac.table.match`. The enable region has no arguments; the value region takes
  one old Entry and returns one complete Entry. Verifiers enforce ownership,
  domain, region shape, result types, and the shared single-writer rule.
- QueueGraph, the Python direct generator, the native QueueGraph C++ generator,
  and typed gfsim stage the full selected update set before commit. Runtime
  validation happens before any Entry is modified, preserving all-or-nothing
  commit.
- Contract epoch remains `0.4`. PYC/RTL continues to reject the provisional
  Table family with `unsupported provisional Table`.

**Deferred work**
- arbitrary integer or Queue-carried masks, CandidateSet algebra, masked reads,
  per-Entry complete-value write lambdas, and domains larger than 64;
- multiple masked writers, scalar/masked arbitration, masked Queue-driven
  endpoints, multidimensional Tables, and cross-object transactions;
- PYC/RTL lowering and cross-backend equivalence.

**Verification**
- Frontend and ACIR tests cover same-Table ownership, mask width, state-only
  form, uniform writes, per-Entry patches, and duplicate writers.
- gfsim tests cover disabled and empty masks, old-state per-Entry evaluation,
  multiple selected Entries, and atomic next-tick visibility.
- A state example is generated and executed through both C++ paths with equal
  final Table state while scalar Table regression tests remain green.

**Source**
- Agentic Circuit masked Table update direction (2026-09-02).

## Decision 0154: epoch 0.4 Table permits disjoint-field writer endpoints

**Status:** Accepted

**Context / Goal**
Issue queues need wakeup logic to update operand-ready fields while selection
clears the valid field of the same Entry in the same tick. A single whole-Entry
writer cannot express these independent state effects without introducing
unrelated allocation priority, firing, or cross-object transaction semantics.

**Decision (strong constraint)**
- A Table may declare multiple scalar or masked writer endpoints exactly when
  their normalized top-level `write_fields` sets are pairwise disjoint.
  Overlap is rejected statically even when address, mask, enable, or predicate
  expressions appear mutually exclusive. Complete Entry writes conflict with
  every other writer.
- Frozen `ac.table.write` and `ac.table.masked_write` require a non-empty,
  duplicate-free `write_fields` string array. Names must resolve to flat struct
  fields. Boolean and integer Entries use the canonical pseudo-field `$entry`;
  complete struct writes enumerate every actual field rather than using a
  wildcard.
- Every value region continues to return a complete Entry. Commit copies only
  the fields declared by that endpoint and preserves every other field from
  old committed state. All endpoints evaluate from one old committed Table
  image, all proposals are validated before mutation, and all disjoint fields
  are merged into one next image published at the tick edge. Endpoint execution
  and transfer order cannot affect the result.
- QueueGraph plans and canonical JSON preserve `write_fields` and repeat field
  existence, uniqueness, non-emptiness, and cross-writer overlap checks. Both
  typed C++ generators emit the corresponding field merge policy.
- gfsim identifies pending proposals by stable writer object ID. Cancellation
  removes only that endpoint's proposal and cannot discard another writer's
  proposal.
- Contract epoch remains `0.4`. PYC/RTL continues to reject every provisional
  Table with `unsupported provisional Table`.

**Deferred work**
- same-field dynamic mutual-exclusion proofs, explicit priority, and allocation
  arbitration;
- `ac.firing`, atomic Queue/Table/Reg transactions, nested field paths,
  multidimensional Tables, and PYC/RTL lowering.

**Verification**
- Frontend and ACIR tests accept scalar/masked disjoint-field combinations and
  reject overlapping or complete writers, malformed `write_fields`, and
  unknown fields.
- gfsim tests cover same-Entry merging, different Entries, disabled or empty
  proposals, old-state visibility, next-tick publication, and writer-local
  cancellation.
- The Issue Queue fixture compiles and runs through direct and native
  QueueGraph C++ generation, merging ready-field wakeup with `valid=false` on
  one Entry while preserving the provisional PYC rejection boundary.

**Source**
- Agentic Circuit field-level multi-writer Table direction (2026-09-03).

## Decision 0155: epoch 0.4 Table match and choose are shared once per Epoch

**Status:** Accepted

**Context / Goal**
One Python `CandidateSet` or `Selection` may feed a masked update, a scalar
update, and a read. Re-expanding its match/choose DAG in every endpoint changes
the authored sharing into repeated full-Table scans and can make endpoint
evaluation order observable in simulator cost.

**Decision (strong constraint)**
- Each authored `table.match(...)` lowers to one dominating top-level
  `ac.table.match`; each authored `table.choose(...)` lowers to one dominating
  top-level `ac.table.choose`. Endpoint policy regions capture those SSA
  results instead of cloning either operation.
- `choose` masks must be produced by `match` on the same Table. Table endpoint
  regions may capture only dominating shared match/choose results belonging to
  their own Table; arbitrary external values remain illegal. The existing
  inline ACIR form remains accepted for compatibility, but new ACPy output is
  canonical shared form.
- QueueGraph preserves each shared match and selection once and represents
  endpoint uses as references. Both C++ paths realize them as lazy caches keyed
  by the complete `Epoch`. The first consumer in an Epoch computes the result;
  all later consumers reuse it. `reset()` invalidates every cache.
- Match, choose keys, reads, and writes observe the same committed Table image.
  Sharing does not add `ac.firing`, priority, backpressure atomicity, or any
  same-field writer exception.
- `policy="first"` uses an empty key region; min/max require one typed key
  region. Contract epoch remains `0.4`, and PYC/RTL keeps rejecting the
  provisional Table family.

**Verification**
- Frontend and ACIR tests prove one shared SSA definition, dominance, same-Table
  provenance, and parser/printer coverage for the empty first-policy key.
- QueueGraph JSON and both C++ generators preserve references without nested
  match/choose expansion. gfsim call-count tests prove one evaluation per Epoch
  and recomputation after Epoch advance or reset.
- The multi-writer Issue Queue example compiles and runs in direct and native
  gfsim while its grant read and valid-clear patch reuse one selection.

**Source**
- Agentic Circuit shared Table selection evaluation direction (2026-09-03).

## Decision 0156: epoch 0.4 Table adds one scalar allocation endpoint

**Status:** Accepted

**Context / Goal**
ROB and Issue Table owners need to install a complete new Entry while independent
wakeup, completion, or removal endpoints update fields in the same tick. Treating
allocation as an ordinary complete field writer would reject that useful overlap;
assigning source-order priority would make the result scheduling-dependent.

**Decision (strong constraint)**
- `table.view(index).allocate(enable=condition, value=new_entry)` declares at
  most one allocation endpoint per Table. It is state-driven, scalar-indexed,
  and requires one complete Entry value. Queue-driven and CandidateSet-masked
  allocation are illegal. The caller supplies the index; the primitive neither
  searches for a free Entry nor checks occupancy or changes `valid` implicitly.
- Frozen ACIR does not add an allocation primitive. `ac.table.write` requires
  `mode "field"` or `mode "replace"`; allocation lowers to `replace`, while
  existing write/patch lowers to `field`. `ac.table.masked_write` requires
  `mode "field"`. Replace declares every struct field in declaration order, or
  `$entry` for a bool/integer Entry.
- Field writers remain pairwise field-disjoint. One replace writer may overlap
  any field writer; a second replace writer is rejected. All expressions observe
  the old committed image. Commit first applies every field proposal to a next
  image, then applies the replace proposal, so allocation wins only when it
  targets the same Entry and endpoint scheduling order is unobservable.
- QueueGraph plans and canonical JSON preserve `mode` and `write_fields` and
  repeat their legality checks. Typed gfsim proposals carry stable writer IDs
  and `FieldMerge`/`Replace` mode; cancellation remains writer-local. Direct and
  native C++ generators emit the same mode. Test-only initial state uses
  `initializeEntry()` only when no proposals are pending rather than disguising
  whole-Entry initialization as a field proposal.
- Contract epoch remains `0.4`. PYC/RTL continues to reject the provisional
  Table family with `unsupported provisional Table`.

**Deferred work**
- multiple allocations, allocation arbitration, automatic free-slot search,
  dynamic same-field mutual exclusion, and explicit ordinary-writer priority;
- `ac.firing`, cross-object atomicity, wakeup-to-select bypass, general
  `ac.reg` head/tail state, multidimensional Tables, and PYC/RTL lowering.

**Verification**
- Frontend and ACIR tests accept replace alongside wakeup/completion/removal
  writers and reject duplicate, masked, Queue-driven, incomplete, or illegal
  modes.
- gfsim tests cover same-Entry replace priority, different Entries, disabled
  allocation, old-state evaluation, and writer-local cancellation.
- Focused Issue Table and ROB examples preserve mode through QueueGraph and
  compile through direct and native typed gfsim C++; PYC retains the stable
  provisional rejection boundary.

**Source**
- Agentic Circuit scalar Table allocation direction (2026-09-03).

## Decision 0157: repository layout integrates Agentic Circuit by responsibility

**Status:** Superseded in part by Decision 0158

**Context / Goal**
The temporary Agentic Circuit import boundary duplicated repository structure,
kept a second runtime primitive tree, and made build, test, packaging, and
integration paths behave like cross-repository interfaces. Git history already
preserves the imported layout, so current development does not need source-tree
compatibility aliases.

**Decision (strong constraint)**
- The repository uses a responsibility-based layout. The pyCircuit and Agentic
  Circuit Python distributions live under `python/`; PYC and ACIR/ACSim compiler
  sources live under `compiler/`; stable PYC C++ and Verilog support lives under
  `library/`; and gfsim lives under `simulator/`.
- Agentic Circuit schemas, tools, examples, and tests live under
  `schemas/agentic-circuit`, `tools/agentic-circuit`,
  `examples/agentic-circuit`, and the corresponding
  `tests/*/agentic-circuit` roots. ACIR/ACSim sources live under
  `compiler/acir`; the `agentic_circuit` Python distribution remains separate
  under `python/agentic-circuit`.
- Reference designs and external-system work are classified explicitly:
  reusable blocks live under `designs/blocks`, supported frontend examples live
  under `examples`, system integrations live under `integrations`, board assets
  live under `platforms`, and imported upstream references live under
  `third_party/references`.
- The former nested Agentic Circuit and runtime directories are removed from the
  current tree. No symlink, forwarding package, path fallback, or duplicate
  Verilog primitive tree preserves those locations. Git history is the only
  compatibility mechanism.
- Current scripts, CI, packaging, documentation, and tests use only the new
  paths. Historical gate logs remain immutable evidence and may retain the
  paths recorded when they were produced.
- Semantic boundaries remain unchanged: ACIR does not become PYC, gfsim does
  not become `libpyc6_runtime`, and the `pycircuit` and `agentic_circuit`
  namespaces remain distinct.

**Verification**
- Layout checks reject deprecated current-tree roots and verify every required
  Agentic Circuit module root.
- Repository CMake configuration, AC G0/G1/G2 orchestration, schema/IR coverage,
  example discovery, and documentation build resolve only canonical paths.
- API hygiene, shell syntax checks, schema catalog generation, and strict
  decision-status validation cover the hard-break configuration surface.

**Source**
- pyCircuit repository responsibility-layout hard break (2026-09-03).

## Decision 0158: consumer designs and integration tooling are out of tree

**Status:** Accepted and implemented

**Supersedes:** the external-system and board placement rules in Decision 0157,
Decision 0142, and Decision 0146.

**Context / Goal**
pyCircuit is a language, compiler, runtime, simulator, and backend framework.
Keeping complete processors, accelerators, board platforms, ISA decoders,
product testbenches, and model-comparison scripts in the framework repository
created path-based exceptions and coupled framework releases to unrelated
consumer infrastructure.

**Decision (strong constraint)**
- The repository owns the `pycircuit` and `agentic_circuit` frontends, PYC and
  ACIR/ACSim dialects and passes, reusable runtime/backend libraries, generic
  examples, framework tests, and framework build/release tooling.
- Complete CPU, NPU, SoC, and board designs are owned by their consumer
  repositories. Linx, Janus, XiangShan, QEMU comparison, ISA decode, FPGA, and
  product-specific visualization or performance scripts are not pyCircuit
  source-tree modules or release gates.
- Consumers depend on a released or commit-pinned pyCircuit Python/CMake
  package and toolchain. Compatibility tests run in the consumer repository and
  record the exact pyCircuit revision.
- Framework code has no consumer-path allowlists, consumer-named runtime
  headers, or conditional semantics selected by a design filename.
- Generic trace, probe, testbench, package, CLI, and plugin/extension contracts
  remain framework APIs. Processor commit bundles and viewer adapters are
  consumer contracts built on those generic surfaces.
- Git history preserves removed in-tree designs and tools for migration. No
  forwarding path, compatibility copy, symlink, or empty placeholder restores
  the old roots.

**Verification**
- The release-layout gate rejects tracked or existing `integrations/`,
  `platforms/`, and the former LinxCore frontend example root.
- Package/runtime inspection rejects Linx/Konata headers and the Python JIT
  uses one design-neutral inline-complexity cap.
- Root CI, examples, performance tooling, and unit tests have no consumer-owned
  design entrypoint.
- AC G0/G1/G2 and the full pyCircuit 6 closure pass without any consumer
  repository checkout.

**Source**
- User direction (2026-09-04): remove LinxCPU/Janus interfaces from the
  pyCircuit framework and decouple designs and tools.

## Decision 0159: pull-request gates are lightweight and full closure is a release gate

**Status:** Accepted and implemented

**Context / Goal**
Building LLVM/MLIR, the complete staged toolchain, both simulation backends,
all examples, and the full AC G0/G1/G2 matrix on every pull-request update
delayed review feedback without changing the release acceptance contract.

**Decision (strong constraint)**
- Required pull-request CI is bounded to changed-file hygiene, repository
  management, documentation, pyCircuit Python unit tests, packaging-helper
  checks, and Python-only Agentic Circuit contract/frontend/CLI-inventory
  tests.
- Pull requests that change semantics, native code, lowering, runtime, or
  packaging still carry the narrowest relevant local test evidence and
  decision mapping. Passing lightweight CI is not evidence that an untested
  native change is correct.
- The release workflow is the only automatic full-closure authority. Before
  publishing, it builds the integrated LLVM/MLIR toolchain, runs ACIR/ACSim and
  gfsim tests, completes AC G0/G1/G2, and runs pyCircuit examples, semantic
  regressions, normal simulations, nightly simulations, strict decision
  status, documentation, package builds, and installed-wheel smoke tests.
- Scheduled nightly and manually requested platform diagnostics may run deep
  subsets for early fault discovery, but they are not pull-request merge gates
  and cannot authorize a release.
- Branch protection requires only the two lightweight job contexts. It must not
  retain deleted heavy-job contexts that make every pull request wait for a
  release-class build.

**Verification**
- The PR workflow contains no LLVM installation, native toolchain build,
  Verilator setup, cross-backend simulation, or wheel build.
- The release workflow blocks package jobs on a successful full AC/PYC closure.
- Repository guidance distinguishes required PR checks, targeted author
  evidence, scheduled diagnostics, and release closure.

**Source**
- User direction (2026-09-04): keep PR validation lightweight and reserve full
  checks for releases.

## Decision 0160: Agentic Circuit exposes exact unsigned widths u1 through u64

**Status:** Accepted and implemented

**Context / Goal**
Architecture models need hardware-sized values such as tags, masks, opcodes,
indices, and packed control fields. Restricting the Python surface to a few
power-of-two widths forced accidental widening and hid truncation behavior.

**Decision (strong constraint)**
- The public frontend defines every fixed unsigned bit type from `ac.u1`
  through `ac.u64`. Width zero, widths above 64, and runtime-computed widths
  are rejected.
- Each `ac.uN` lowers to an exact `iN` ACIR/PYC value and may be used as a
  scalar Queue payload or an `@ac.struct` field.
- Addition, subtraction, multiplication, bitwise AND/OR/XOR/NOT, and logical
  left/right shift preserve width. Binary operands must have identical widths;
  a right-side integer literal is typed from its left operand. No implicit
  widening/narrowing is inserted.
- Equality is width-exact and relational comparison of `ac.uN` values is
  unsigned in the Python, QueueGraph, gfsim, and PYC paths.
- Results use circuit semantics: arithmetic and bitwise results are truncated
  modulo (2^N); a shift amount greater than or equal to (N) produces zero.
- ACIR owns verifier enforcement through typed `ac.var.*` operations. The
  QueueGraph-to-PYC lowering emits the corresponding `pyc.*` operations.
  gfsim uses `gfsim::UInt<N>` for every width so direct and native C++
  generators preserve truncation independently of C++ integer promotions.
- Existing signed alias names remain import-compatible, but ACIR does not yet
  preserve signedness as a distinct type. Their relational operators therefore
  use the same signless unsigned lowering for now; signed comparison semantics
  require a separate type-system decision.

**Verification**
- Public API tests cover all 64 names and reject out-of-range widths.
- Frontend tests cover non-power-of-two fields, same-width enforcement,
  constants, structures, and all supported operations.
- ACIR parser/verifier tests cover the new bit operations and invalid payloads.
- QueueGraph C++ and PYC code-generation tests cover an `i3` structure and
  compile the generated gfsim C++.
- gfsim unit tests prove modulo truncation and width-bounded shift behavior.

**Source**
- User direction (2026-09-05): expose `u1, u2, u3, ..., u64` as exact circuit
  bit types that compose into classes/structures and support bit operations.

## Decision 0161: semantic primitives select qualified RTL only in the Verilog backend

**Status:** Accepted and implemented

**Context / Goal**
PR #29 demonstrated that a large external RTL catalog can accelerate hardware
construction, but vendor module names, crawler output, license mixtures, and
implementation parameters are not a stable language IR.  Python and canonical
PYC need one semantic contract while Verilog may instantiate a qualified
handwritten implementation without lowering it into gates.

**Decision (strong constraint)**
- The repository separates a vendor-neutral semantic registry under
  `schemas/primitives` from the replaceable implementation catalog
  `library/verilog/rtl_catalog.json`.  Only semantic IDs may appear in public
  Python and canonical PYC.
- `pyc.priority_encode` is the first admitted semantic primitive.  It returns
  exact `index` and `valid` results, supports low-first and high-first order,
  and participates in combinational dependency and logic-depth analysis.
- C++ simulation retains the semantic operation and executes reference
  behavior.  The Verilog-only `pyc-select-rtl-primitives` pass selects exactly
  one highest-priority qualified candidate and rewrites it to internal
  `pyc.rtl.comb`.  Equal highest priorities are an error, not an implicit
  implementation-ID tie break.
- `pyc.rtl.comb` is backend-owned IR.  Source input containing it is rejected.
  Its verifier requires typed arity, disjoint legal port names, integer
  parameters, normalized relative sources, lowercase SHA-256 digests, license
  IDs, and a catalog fingerprint.
- Catalog entries are selectable only with `qualification.status=validated`.
  Selection verifies every source digest against the catalog directory.
  `pycc --out-dir` emits the minimal selected source closure once, rejects
  unresolved include directives, records versioned implementation definitions
  separately from parameterized bindings, and preserves the union across
  multi-module Python builds.
- Repository-owned admitted RTL uses BSD-3-Clause.  The Solderpad-licensed
  BaseJump files evaluated from PR #29 are not imported or relicensed.
- Agentic Circuit exposes the same operation as
  `ac.priority_encode(value, order=...).index/.valid`.  ACPy emits one shared
  `ac.var.priority_encode`; QueueGraph lowers it to the semantic PYC operation,
  while gfsim uses `gfsim::priorityEncode` and a dedicated SimQueue
  `PriorityEncode` block.
- Stateful, handshake, memory, and CDC candidates from PR #29 are not admitted
  by this decision.  They require distinct effect-class IR and the inferred
  prepare/publish/no-fail commit contract from `D-RULE-LOWERING-001`; public
  Python does not gain `atomic`, `check`, `reserve`, `push`, or `pop` syntax.

**Verification**
- Widths 1, 4, 8, and 13; low/high order; zero, one-hot, and multi-hot inputs.
- PYC verifier, raw selected-IR rejection, source-digest rejection, ambiguous
  selection rejection, C++ reference execution, selected RTL lint/simulation,
  manifest/source closure, and multi-module binding union.
- Agentic Python/ACIR parser and verifier, native/direct QueueGraph C++,
  QueueGraph-to-PYC, generated C++ compilation, and SimQueue execution.

**Source**
- User direction (2026-09-05): treat qualified PR #29 blocks as PYC semantic
  primitives with simple parameterized Python APIs, MLIR/JIT selection,
  handwritten-Verilog lowering, and Agentic SimQueue models.

## Decision 0162: inferred state transitions use prepare, publish, and no-fail commit groups

**Status:** Accepted and implemented for the gfsim runtime substrate

**Context / Goal**
An inferred rule may consume several Queue tokens, update one Table footprint,
and produce tokens on a selected subset of output Queues.  Independent
`proposePush`, `proposePop`, and Table writer calls can leave partial proposals
when a later resource rejects the same transition.  Rollback after publication
is not an acceptable circuit execution model.

**Decision (strong constraint)**
- Public Python continues to describe functional rule intent.  It does not
  expose `atomic`, `check`, reservation, `push`, `pop`, commit, or rollback
  mechanics.  MLIR inference, check materialization, handshake construction,
  scheduling, and lowering own those details.
- The gfsim resource protocol for one inferred transition is
  `prepare -> publish -> no-fail Xfer commit`.  Prepare changes no committed
  state and may be cancelled by the stable transition owner.  Publication is
  legal only after every selected Queue endpoint and the complete Table write
  footprint are reserved by that same commit group. Published group proposals
  are sealed against transition-local reset and writer cancellation, which
  cannot roll back one resource after the other group members have published.
- A Queue commit-group reservation is exclusive against ordinary proposals and
  other commit groups until it is published or cancelled.  Queue transfer
  order and capacity remain the existing committed-state contract.
- Commit-group Table preparation uses the actual per-Epoch index set,
  write-field set, and replace/field-merge mode.  Prepared writes to different
  entries are disjoint; field-merge writes to the same entry are compatible
  only for disjoint fields; two prepared replaces conflict only where their
  index sets overlap.  The existing deterministic rule that replace is applied
  after field merges remains the defined result for mixed replace/merge
  overlap.  Independent provisional epoch 0.4 Table endpoints keep Decision
  0154's stricter static field-set and whole-replace conflict rule; this runtime
  API does not silently promote them into independently schedulable rules.
- The internal transition constructor requires an explicit Table write mode;
  a whole-Entry merge policy cannot accidentally masquerade as a single-field
  merge footprint. Every Queue/Table resource owns and performs its own Xfer
  commit. The transition clears only its firing-local state, so committed-source
  observation and activation do not depend on ObjectId order.
- A generated `QueueTableTransition` first evaluates one functional branch
  from the committed snapshot, then checks readiness for exactly that branch,
  reserves all selected outputs, inputs, and Table locations, and publishes
  the complete group.  Backpressure on the chosen route stalls the transition;
  it never selects a different functional branch because another output is
  ready.
- Unexpected unpublished reservations at a Queue transfer barrier and any
  publish-after-prepare contract violation produce stable runtime failures.
  Ordinary resource unavailability cancels reservations and leaves
  `S(t+1) = S(t)` without a diagnostic.
- `QueueAtomicTransform` and `QueueBarrier` use the same internal commit-group
  protocol so duplicate endpoints cannot create partial Queue proposals.
- This decision accepts the gfsim runtime substrate and its Table-plus-Queue
  ROB-style vertical examples.  It does not by itself claim that stateful
  `@ac.rule` capture, branch normalization, arbitration, Reg effects, or PYC/RTL
  lowering is complete; those remain verifier-owned follow-on stages of
  `D-RULE-LOWERING-001`.

**Verification**
- Queue tests cover successful prepare/publish, cancellation, exclusive
  reservation, and duplicate input/output endpoints.
- Table tests cover dynamic index and field footprints, disjoint writers,
  conflicting writers, writer-local cancellation, and replace-after-merge.
- Transition tests cover allocate/replace with input and output Queues, masked
  patch with an input Queue, read-and-remove under output backpressure, selected
  route backpressure without rerouting, cancellation after a Table conflict,
  sealed publication under local reset, and ObjectId-order-independent Table
  activation.
- The complete gfsim C++ test binary remains green.

**Source**
- User direction and PTO-ISA/pyCircuit issue 28 (2026-09-05): infer atomic
  state transitions below the Python surface and prove them with a ROB-shaped
  gfsim example.

## Decision 0163: the first stateful rule slice lowers one Table replace with its Queue transfer

**Status:** Accepted and implemented

**Context / Goal**
Decision 0162 established the no-partial-commit runtime protocol but did not
connect the simple Python rule surface to Table state. The first compiler slice
must prove that the frontend can remain functional and compact while MLIR owns
effect discovery, resource handshake, scheduling, and grouped lowering.

**Decision (strong constraint)**
- A phase-one stateful rule has one Table parameter followed by one immutable
  Queue payload parameter. Its body may bind one committed Table observation,
  performs exactly one complete Entry assignment, and returns one payload:

  ```python
  @ac.rule
  def install(rob, entry):
      old = rob[entry.index]
      rob[entry.index] = entry
      return old
  ```

  The author does not spell Queue consumption/production, readiness, checks,
  reservation, commit, rollback, or atomic regions.
- The call site is `outgoing = install(rob, incoming)`. The input and output
  Queue payload and the Table Entry type are identical in this slice. The
  Table assignment is a complete replace; field patch, multiple proposals,
  multiple Tables, optional outputs, and CFG branches remain rejected.
- A dynamic `ac.uN` index is accepted only when the Table contains exactly
  `2^N` entries, which statically discharges bounds. A constant index must be
  in range. Other dynamic shapes remain pending on executable checked IR and
  fail before Frozen ACIR.
- Raw ACIR represents state intent with firing-local `ac.table.propose`. Its
  verifier requires direct `ac.rule`/`ac.firing` ownership, one resolved and
  visible Table, complete replace fields, matching Entry type, and a statically
  safe index. It is not an independent Table endpoint and cannot commit alone.
- The existing staged rule passes infer the Queue and Table effects, establish
  an empty dynamic-check contract, materialize
  `ready_valid_1x1_table`; later Decision 0173 moves publication into stable
  arbitration and replaces exclusive scheduling with inferred lexical priority,
  discharge markers, and lower to marker-free `ac.firing`. A stateful firing is
  never canonicalized to `ac.transform`.
- Phase-one scheduling requires exclusive write ownership of the Table. Any
  other rule proposal, scalar Table writer, or masked writer is diagnosed;
  source order does not become arbitration.
- QueueGraph preserves the stateful `firing` block, including Table identity,
  index/value SSA identities, fields, mode, and output expression. Native C++
  lowers it to `gfsim::QueueTableTransition`, whose Queue and Table effects use
  Decision 0162 commit groups. Generated models expose committed Table state
  through a const-only architecture-model inspection accessor; callers cannot
  create or cancel proposals through that surface.
- The Frozen QueueGraph boundary re-verifies the complete MLIR module before
  extraction, then independently checks Queue/Table type equality, constant or
  full-domain index safety, same-Table observations, and exclusive write
  ownership. A forged digest or hand-authored firing cannot bypass the dialect
  verifier by reaching the plan or C++ generator directly.
- PYC and RTL continue to reject this provisional Table graph with
  `unsupported provisional Table`. This slice is gfsim execution evidence, not
  authorization to admit stateful RTL primitives or backend-only semantics.

**Verification**
- Python tests cover the simple read/assignment/return surface, emitted typed
  proposal, absent implementation vocabulary, payload/Entry matching, and
  fail-closed non-power-of-two dynamic indexing.
- ACIR tests cover proposal parsing/verifiers, staged effects/handshake/schedule,
  marker-free stateful firing, topology freeze, generated C++ compilation, and
  stable PYC rejection.
- The end-to-end example writes one Table location twice and observes zero then
  the first committed value while the final Table contains the second value.
  Queue consumption, output production, and Table replace therefore cross the
  same generated commit-group path.

**Deferred work**
- multiple input/output Queues, multiple or masked/field proposals, CFG joins,
  functional guards and mutually exclusive branches, dynamic checked IR,
  explicit arbitration, Reg effects, and a full circular ROB;
- PYC/RTL lowering for stateful rules and qualification of stateful PR #29
  implementation candidates.

**Source**
- User direction and PTO-ISA/pyCircuit issue 28 (2026-09-05): keep Python
  state authoring simple and move type/effect/check/handshake complexity into
  MLIR passes and lowering.

## Decision 0164: population count is a semantic primitive with qualified RTL selection

**Status:** Accepted and implemented

**Context / Goal**
Agentic Circuit already exposed population count, but QueueGraph-to-PYC expanded
it into per-bit extracts, extensions, and an adder tree carrying ad hoc
implementation metadata. PR #29 also supplied external population-count RTL.
The language needs one stable semantic operation while the Verilog backend may
select a qualified implementation without leaking module names into Python or
canonical PYC.

**Decision (strong constraint)**
- `pyc.popcount` accepts one `iN` input and returns
  `i(max(1,ceil(log2(N+1))))`. The result is the number of asserted input bits;
  zero maps to zero and an all-ones value maps to `N`.
- Structural pyCircuit exposes `Circuit.popcount(value)`. Cycle-Aware Signal
  exposes `value.popcount()` and `pycircuit.popcount(value)`, preserving the
  aligned input cycle. These APIs are parameterized by the input type and do
  not accept an implementation name.
- Agentic Circuit keeps `ac.popcount(value)`. ACIR uses
  `ac.var.popcount`; QueueGraph-to-PYC now emits one `pyc.popcount` instead of a
  lowered extract/add tree. QueueGraph C++ uses `gfsim::populationCount`, and
  gfsim provides a typed `Popcount<Width>` SimQueue block.
- PYC owns exact input/result verification, combinational dependency, and
  logarithmic logic-depth cost. C++ and direct Verilog emission provide the
  semantic reference behavior.
- The Verilog-only primitive selection pass may rewrite `pyc.popcount` to
  backend-owned `pyc.rtl.comb`. The selected candidate binds `WIDTH` and
  `COUNT_WIDTH`, uses ports `in_value` and `count`, and participates in the
  same qualification, digest, ambiguity, source-closure, and manifest rules as
  Decision 0161.
- The admitted implementation is repository-owned
  `pyc_popcount_primitive.v` under BSD-3-Clause. The Solderpad-licensed
  BaseJump `bsg_popcount.sv` evaluated in PR #29 is not imported or relicensed.
- The qualified width range is 1 through 64. Wider PYC values retain semantic
  C++ behavior but have no admitted RTL candidate and fail selection closed.

**Verification**
- PYC verifier tests reject an incorrect count width.
- Widths 1, 4, 13, and 64 are checked in gfsim and Icarus; the 13-bit PYC C++
  reference and selected RTL both produce 0, 5, and 13 for representative
  inputs.
- Python tests prove exact width, same-cycle behavior, and absence of vendor
  names. ACIR lit proves one semantic PYC op and the existing Agentic Verilog
  path remains functional.
- Selection tests cover catalog ownership, parameters, BSD source digest,
  emitted source closure, and manifest bindings.

**Source**
- User direction and PTO-ISA/pyCircuit PR #29 (2026-09-05): promote reusable
  parameterized blocks into semantic PYC IR, select qualified handwritten RTL
  during lowering, and provide an Agentic SimQueue realization.

## Decision 0165: leading-zero count is a semantic primitive with defined zero input

**Status:** Accepted and implemented

**Context / Goal**
PR #29 contains several vendor CLZ/LZC implementations with different output
and all-zero conventions. Python and canonical PYC need one portable contract;
implementation abbreviations, module names, and vendor-specific valid flags
must not leak into the language.

**Decision (strong constraint)**
- `pyc.count_zeros` with `direction = "leading"` accepts one `iN` input and returns
  `i(max(1,ceil(log2(N+1))))`. It counts consecutive zero bits starting at the
  most-significant bit. An MSB-one input returns zero and an all-zero input
  returns `N`.
- Structural pyCircuit exposes `Circuit.count_leading_zeros(value)`.
  Cycle-Aware Signal exposes `value.count_leading_zeros()` and
  `pycircuit.count_leading_zeros(value)`, preserving the input cycle. Agentic
  Circuit exposes `ac.count_leading_zeros(value)` and lowers it through
  `ac.var.count_zeros` with static leading direction.
- QueueGraph-to-PYC emits one semantic op. QueueGraph C++ and direct JIT use
  `gfsim::countLeadingZeros`; gfsim also provides a typed
  `CountLeadingZeros<Width>` SimQueue block.
- PYC and ACIR verifiers own exact result-width enforcement. Dependency and
  logic-depth analysis charge the balanced zero-detect/count tree rather than
  an expanded Python or backend chain.
- Verilog selection may rewrite the semantic op to backend-owned
  `pyc.rtl.comb`. The admitted 1-through-64 candidate binds `WIDTH` and
  `COUNT_WIDTH` and participates in the same fail-closed catalog, digest,
  ambiguity, source-closure, and manifest contract as Decisions 0161 and 0164.
- The admitted repository-owned BSD-3-Clause implementation is generalized by
  Decision 0166 into `pyc_count_zeros_primitive.v`. BaseJump/PULP Solderpad
  sources and Vortex Apache wrappers from PR #29 are design references only
  and are not imported or relicensed.

**Verification**
- Width-one, non-power-of-two, power-of-two, MSB-one, mixed, and all-zero cases
  agree across PYC C++, selected RTL, Agentic direct JIT, and gfsim.
- Python tests prove exact width, same-cycle behavior, and vendor-neutral IR.
  ACIR lit proves one semantic PYC op; selection tests prove digest-closed BSD
  source and width-65 fail-closed behavior.

**Source**
- User direction and PTO-ISA/pyCircuit PR #29 (2026-09-05): normalize reusable
  combinational blocks as parameterized semantic IR with selected RTL and an
  Agentic SimQueue realization.

Decision 0166 supersedes the standalone canonical
`pyc.count_leading_zeros` spelling and implementation identity while
preserving this decision's public leading-zero semantics.

## Decision 0166: zero count is one static-direction semantic family

**Status:** Accepted and implemented

**Context / Goal**
PR #29's LZC candidates commonly parameterize leading versus trailing
direction. Duplicating complete ACIR, PYC, gfsim, catalog, and RTL stacks for
the two directions would turn an implementation parameter into two unrelated
compiler concepts.

**Decision (strong constraint)**
- Python keeps the readable `count_leading_zeros` and `count_trailing_zeros`
  helpers. They infer width from the operand, accept no implementation knobs,
  preserve Cycle-Aware timing, and both map all-zero input to `N`.
- Canonical ACIR uses `ac.var.count_zeros` and canonical PYC uses
  `pyc.count_zeros`. Each carries exactly one compile-time `direction` enum:
  `leading` or `trailing`. Runtime-computed direction and noncanonical strings
  are rejected by verifiers.
- QueueGraph freezes direction in the expression plan. Direct Agentic JIT and
  generated C++ use `gfsim::countLeadingZeros` or
  `gfsim::countTrailingZeros`; both are projections of one
  `CountZeros<Width, Direction>` SimQueue template.
- Verilog selection introduces one `pyc.rtl.comb` semantic/implementation
  family and binds direction to `DIRECTION_LOW`. One repository-owned
  `pyc_count_zeros_primitive.v` implements both directions with the same
  padded balanced tree and stop-sentinel depth contract.
- The semantic result width remains `max(1,ceil(log2(N+1)))`; the qualified
  width range remains 1 through 64 and wider Verilog selection fails closed.

**Verification**
- Widths 1, 4, 13, and 64 cover both directions, mixed values, endpoint-one
  values, and all-zero input in gfsim and Icarus.
- PYC C++ and selected RTL return identical leading/trailing results from the
  same input. Selection manifests contain two bindings of one implementation,
  differing only in `DIRECTION_LOW`.
- Python, ACIR, QueueGraph, JIT, PYC, source-closure, and Verilator tests prove
  that direction remains static and implementation-neutral until selection.

**Source**
- User direction and PTO-ISA/pyCircuit PR #29 (2026-09-05): expose simple
  parameterized Python while MLIR passes select reusable RTL building blocks.

## Decision 0167: pure rules infer atomic multi-input Queue handshakes

**Status:** Accepted and implemented

**Context / Goal**
DavinciOO L3 modules frequently join independently backpressured Queue inputs.
Requiring Python authors to inspect readiness or spell pop, push, reservation,
or commit mechanics would duplicate protocol code and make partial consumption
possible. The rule frontend should remain serial and functional while MLIR
materializes the transaction boundary.

**Decision (strong constraint)**
- A pure `@ac.rule` accepts one or more positional Queue payload parameters and
  retains one total return path and one output Queue in this slice. Invocation
  supplies one distinct Queue per parameter. Input payload types may differ;
  the result currently preserves the primary input payload type.
- Python contains no Queue readiness, pop, push, reservation, atomic, commit,
  or rollback API. Each parameter denotes the committed head payload observed
  at the tick start.
- Transient `ac.rule` is variadic in its Queue operands and block arguments.
  MLIR infers the shared input-consume/output-produce effect set and
  materializes `ready_valid_Nx1`, where `N` is the input arity.
- The rule fires only when every input token and the output capacity are
  available. The closed pure firing canonicalizes to one variadic
  `ac.transform`; QueueGraph generates one typed `QueueAtomicTransform` whose
  prepare/publish/no-fail commit group consumes every input and produces the
  output together.
- Every input Queue must have an exclusive consuming rule use in this slice.
  Observers remain non-consuming. CFG branches, optional or multiple outputs,
  and stateful multi-Queue rules remain deferred.
- Single-input pure rules and the existing one-Table/one-Queue stateful rule
  keep their existing IR and runtime contracts.

**Verification**
- Python frontend tests prove homogeneous and heterogeneous Queue arguments
  lower to one variadic rule without public handshake or atomic operations and
  reject invocation arity or duplicate Queue arguments.
- ACIR verifier and lowering tests prove `ready_valid_2x1`, marker discharge,
  variadic transform canonicalization, topology freeze, typed gfsim generation,
  and C++ compilation.
- Existing rule, Table-rule, atomic-transform, and complete ACIR lit suites
  remain green.

**Source**
- User direction and PTO-ISA/pyCircuit issue #28 (2026-09-05): keep the
  parameterized serial frontend simple, infer atomic Queue conditions in MLIR,
  and generate efficient reusable backend blocks.

## Decision 0168: stateful rules infer atomic heterogeneous multi-input transactions

**Status:** Accepted and implemented

**Context / Goal**
DavinciOO L3 owners commonly need one primary state-entry request plus metadata,
completion, lease, or control tokens with different payload types. These Queue
consumptions, one Table replacement, and the result must share one commit
without exposing Queue mechanics in Python.

**Decision (strong constraint)**
- A stateful `@ac.rule` accepts one Table parameter followed by one or more
  positional Queue payload parameters. Invocation supplies the owner Table and
  one distinct Queue per payload parameter.
- The primary Queue payload and single output payload match the Table Entry.
  Additional Queue payloads may use different fixed types and participate in
  ordinary serial expressions used to compute the replacement or result.
- MLIR infers `ready_valid_Nx1_table`, all Queue consume/produce effects, and
  the Table replace footprint. The stateful firing remains one internal
  `ac.firing`; it is not canonicalized into a pure transform.
- QueueGraph preserves every typed input in order and generates one
  `QueueTableTransition<..., tuple<Inputs...>, tuple<Entry>>`. Its
  prepare/publish/no-fail commit group reserves every selected Queue and the
  Table footprint before publishing any effect.
- A missing input, output backpressure, or Table reservation conflict leaves
  every input Queue and the Table unchanged. Additional state proposals,
  field/masked writes, CFG branches, optional or multiple outputs, Reg effects,
  and conflict arbitration remain deferred.

**Verification**
- Frontend tests prove a Table plus heterogeneous `Entry` and `Delta` Queues
  lower without public readiness or atomic syntax.
- ACIR lit proves `ready_valid_2x1_table`, typed QueueGraph preservation,
  variadic gfsim generation, and generated C++ compilation.
- The generated-model integration test proves missing-input and output-full
  stalls consume neither Queue and update no Table state, followed by one
  all-resource commit when capacity becomes available.

**Source**
- User direction and PTO-ISA/pyCircuit issue #28 (2026-09-05): move atomic
  resource checks into MLIR while keeping parameterized serial Python simple.

## Decision 0169: variables use inferred lifetime and update properties

**Status:** Accepted and implemented for the first MLIR analysis slice

**Context / Goal**
The Python frontend should describe ordinary values, class/module fields,
structs, rules, and lexical scopes rather than hardware resources. Replacing
`persistent` with `mutable` would conflate lifetime with update permission:
compile-time configuration may be persistent and immutable, while a local
Python name may be reassigned yet still lower to temporary immutable SSA.

**Decision (strong constraint)**
- Variable analysis tracks two orthogonal properties. `lifetime` is
  `static`, `temporary`, or `persistent`; `update` is `immutable` or
  `assignable`. Unknown is an internal lattice state, not a public spelling.
- Python does not gain `Input`, `Output`, `Queue`, `Table`, or `Reg` variable
  annotations and does not spell either analysis property. Rule parameters,
  returns, locals, module fields, and lexical def-use provide the source facts.
- `ac.var` is the single ACIR variable-value concept for expressions and
  inferred state.
- `const` values are static and immutable. Rule/process parameters and local
  expression results are temporary immutable snapshots. Scope-owned state is
  persistent and assignable through next-state proposals; its committed value
  observed in one activation remains immutable.
- `ACDataFlowAnalyzer`, built on the MLIR dataflow framework, owns propagation
  across SSA def-use, regions, and calls. The underlying solver is private.
  Lexical definition scope determines owner identity. Later storage and
  transport selection may realize persistent variables as scalar storage,
  dense arrays, associative state, memories, or Queues without exposing those
  choices in Python.
- The first implementation provides a reusable Variable analysis lattice and
  classifies ACIR constants, temporary SSA/block arguments, lexical owners,
  and existing owned-state operations. `ac-infer-rule-effects` invokes
  `ACDataFlowAnalyzer` and requires every rule argument to be a temporary
  immutable `ac.var` snapshot before effect inference. It introduces no marker
  or backend behavior change.

**Verification**
- Native analysis tests prove an in-scope constant is static immutable, a
  transform argument and result are temporary immutable, and existing owned
  state is persistent assignable with its declared owner.
- Existing ACIR analysis, rule, QueueGraph, and gfsim gates remain unchanged.

**Source**
- User direction (2026-09-05): make the frontend variable-, class-, struct-,
  and scope-oriented; infer persistence, temporary values, ports, resources,
  and atomic checks in MLIR rather than adding hardware-named Python types.

## Decision 0170: persistent fields use the existing ac.var family before storage selection

**Status:** Accepted and implemented for zero-initialized scalar and flat-struct state

**Context / Goal**
Python module/class fields need a generic persistent-variable path without
exposing register, Table, Queue, or memory choices. Expression values and state
must remain one `ac.var` family so storage selection does not leak into Python.

**Decision (strong constraint)**
- `ac.var` remains the single variable family. Internal `ac.var.decl` gives a
  persistent lexical identity, `ac.var.read` observes its committed immutable
  snapshot, and firing-local `ac.var.assign` proposes its next value.
- These operations are compiler IR targets for inferred Python class/module
  fields; they are not new Python constructors or annotations.
- `ac-lower-variable-state` runs before rule effect inference. Storage
  selection lowers the generic variable operations to a concrete committed
  implementation, after which the ordinary rule, transaction, QueueGraph, and
  gfsim pipeline remains unchanged.
- The first storage-selection slice supports zero-initialized scalar integers
  and flat structs and selects a one-entry committed state array. Reads lower
  to index-zero committed observation and assignments lower to complete
  index-zero replacement in the owning firing. Struct replacement preserves
  declaration-order field footprints.
- Non-zero initialization, nested struct/array/map fields, multiple assignments,
  field footprints, conflict arbitration, and direct scalar runtime storage
  remain follow-up selections. They must extend the generic ac.var semantics,
  not add hardware-named frontend types.

**Verification**
- Verifier tests reject mismatched initialization, mismatched reads, and
  assignment outside a rule/firing.
- Storage-selection lit proves `ac.var.decl/read/assign` disappear before rule
  closure, lower to one committed state owner, generate grouped gfsim C++, and
  compile as C++20.
- `ACDataFlowAnalyzer` classifies the declaration as persistent assignable and
  keeps read results as immutable SSA values.

**Source**
- User direction (2026-09-05): keep `ac.var` as the one variable concept and
  infer persistent versus temporary behavior in MLIR.

## Decision 0171: typed system signatures infer external Queue boundaries

**Status:** Accepted and implemented

**Context / Goal**
System authors should describe external values with ordinary typed parameters
and returns. Requiring `source(...)` and `sink(...)` in Python exposes graph
plumbing that follows directly from the callable boundary and distracts from
the serial functional model.

**Decision (strong constraint)**
- A non-`const` system parameter denotes one external runtime value. Its type
  annotation fixes the payload type; the frontend inserts the internal source
  boundary before the system body is lowered.
- A returned Queue value denotes one external result. A `tuple[...]` return
  annotation and tuple return denote multiple ordered results; the frontend
  inserts one internal sink per result.
- Return arity and payload types are checked against the system annotation.
  These checks do not expose Queue readiness, pop, push, or sink objects to
  Python.
- Existing explicit `source(...)`/`sink(...)` inputs remain accepted only as a
  transitional regression surface. New examples and the eventual ROB/ISQ use
  typed parameters and returns.
- ACIR, QueueGraph, and gfsim retain explicit source/sink nodes as compiler and
  runtime boundaries. Their existence below Python is not a second authoring
  model.

**Verification**
- Frontend tests prove a typed single-input/single-result rule and a typed
  multi-input/multi-result system insert the expected internal boundaries and
  reject return arity or payload mismatches.
- The generated-model integration test compiles and runs the inferred-boundary
  pipeline through rule lowering, QueueGraph, and typed gfsim C++.

**Source**
- User direction (2026-09-05): Python expresses scope and ordinary typed
  variables; MLIR infers boundaries, backpressure, and transaction mechanics.

## Decision 0172: persistent Python lists remain shaped ac.var state until storage selection

**Status:** Accepted and implemented for one-dimensional zero images

**Context / Goal**
A circular ROB needs dynamically indexed persistent entries, but Python should
not name a Table or register bank. A fixed Python list is the natural source
concept; its physical storage and transaction behavior belong to MLIR.

**Decision (strong constraint)**
- A lexical declaration such as `entries: list[Entry] = [0] * 8` defines one
  persistent assignable variable with a statically inferred one-dimensional
  shape. The list is not a Queue collection and introduces no new Python
  hardware type.
- Shaped state remains in the existing family: `ac.var.decl` carries the shape,
  `ac.var.read_element` observes one immutable committed element, and
  `ac.var.assign_element` proposes one next-state element update.
- `ac-lower-variable-state` selects the current dense committed realization by
  lowering the logical operations to `ac.table`, `ac.table.get`, and
  `ac.table.propose`. This is a compiler choice, not an author-visible Table.
- The first slice accepts a non-empty static zero image and one-dimensional
  shape. Dynamic index width must exactly cover a `2^N` shape. Later executable
  range-check IR may relax this restriction without changing Python syntax.
- gfsim commits only touched entries; no per-tick whole-list copy is permitted.

**Verification**
- ACIR verifier tests reject invalid shapes, scalar reads of shaped state, and
  dynamic indices whose width exceeds the list domain.
- Storage-selection lit proves all shaped variable operations disappear,
  become one dense committed state owner, and compile through QueueGraph/gfsim.
- Frontend and generated-model tests prove the same Python list entry can be
  replaced twice and the second transaction observes the first committed value.

**Source**
- User direction (2026-09-05): use Pythonic variable/data-structure concepts;
  infer Table/map/register realizations and atomic mechanics in MLIR.

## Decision 0173: stateful candidates publish only in the arbitration phase

**Status:** Accepted and implemented in the gfsim transaction substrate

**Context / Goal**
Multiple ROB rules will observe one immutable committed snapshot and may target
overlapping state. If a transition reserves or publishes during Work, whichever
object happens to be visited first acquires the state footprint. That hidden
ordering is incompatible with compiler-derived scheduling.

**Decision (strong constraint)**
- `QueueTableTransition::doWork` may only inspect committed Queue/Table state,
  evaluate the functional policy, and retain an immutable candidate plan. It
  must not reserve or publish Queue or Table effects.
- `doArbitrate` rechecks the selected output capacity, then performs the full
  Queue input/output and Table prepare/publish protocol. A failed reservation
  cancels the whole group and leaves every committed resource unchanged.
- Arbitration order is explicit stable dispatch order. Candidate evaluation
  order cannot affect the winner. Non-conflicting footprints may both publish;
  conflicting candidates are retried from fresh committed state later.
- `doXfer` clears both candidate and committed-transition bookkeeping. Reset
  cancels prepared state and cannot partially roll back an already published
  group.
- This runtime substrate does not by itself authorize shared state in Frozen
  ACIR. MLIR must first infer/freeze priorities and read/write conflicts before
  the existing exclusive-writer verifier is relaxed.

**Verification**
- Existing Table/Queue transition tests call the explicit arbitration phase and
  retain atomic backpressure, cancellation, mask, replacement, retire, and
  routing behavior.
- A two-transition regression evaluates candidates in reverse order, then
  arbitrates in stable priority order and proves only the selected conflicting
  write consumes its input and commits.
- The complete gfsim suite and generated stateful model integrations remain
  green.

**Source**
- Architecture review for the circular ROB implementation (2026-09-05): make
  compiler scheduling observable at Arbitrate rather than accidental in Work.

## Decision 0174: ACDataFlowAnalyzer freezes shared-state footprints and lexical priority

**Status:** Accepted and implemented for whole-entry replacement rules

**Context / Goal**
Once stateful publication occurs only in Arbitrate, multiple rules may safely
target one persistent list if the compiler preserves their logical access sets
and an explicit deterministic priority. Merely deleting exclusive-writer checks
would not prove that generated execution follows Python's serial order.

**Decision (strong constraint)**
- `ACDataFlowAnalyzer` derives an ordered footprint for each Table-backed
  `ac.var` access. A footprint records resource identity, read/replace access,
  static versus dynamic index classification, and ordered write fields.
- Rule-effect inference stores those footprints as structured MLIR attributes.
  Schedule resolution assigns a non-negative lexical priority to every rule;
  stateful rules use `table_lexical_priority` rather than exclusive ownership.
- Rule lowering preserves priority and footprints on internal `ac.firing`.
  QueueGraph validates that firing priorities remain strictly ordered before
  code generation. Stable generated dispatch order is therefore the executable
  arbitration order.
- Multiple `ac.table.propose` endpoints created from inferred variables may
  share one Table. Explicit legacy Table writers remain incompatible with those
  firings until they also adopt candidate-only Work semantics.
- Runtime dynamic index/field footprints decide whether candidates conflict.
  Reverse Work evaluation cannot change which lexical candidate commits.
- This slice still permits only one whole-entry proposal per rule. It does not
  yet authorize multiple state owners, conditional proposals, or CFG paths.

**Verification**
- Native analysis tests prove ordered read/replace footprint inference and
  temporary/static immutable index classification.
- MLIR lowering tests prove structured footprints and priorities survive on
  Frozen firing IR.
- The `shared_indexed_rules.py` generated-model test evaluates Work in reverse,
  arbitrates in stable order, and proves the lower-priority same-entry input is
  retained until it observes and replaces the first committed value.

**Source**
- User direction and ongoing circular ROB flow review (2026-09-05): derive
  checks and schedules with MLIR dataflow rather than Python readiness logic.

## Decision 0175: stateful rules support consume-only and guarded state-driven transactions

**Status:** Accepted and implemented for one condition and zero/one result

**Context / Goal**
A ROB completion port consumes an input even when it returns no value, while
retirement is driven by committed state and may have no Queue input. Requiring
dummy Queues, polling tokens, or frontend readiness checks would distort both
the model and its performance.

**Decision (strong constraint)**
- A stateful rule may have inputs and zero outputs. MLIR infers an `Nx0_table`
  handshake; Queue consumption and the state proposal remain one commit group.
  Python emits neither a dummy return nor a sink.
- A stateful rule may have zero inputs and one output. Its policy observes the
  committed state snapshot and produces a candidate only when its typed
  functional condition is true.
- The first frontend CFG slice accepts one `if` without `else` around the
  indexed state assignment and optional return. It emits
  `ac.rule.condition`; lowering retains `ac.firing.condition` in Frozen ACIR.
- Rules without an authored condition receive an explicit compiler-inserted
  true condition. Pure rules prove that condition and erase it only when
  canonicalizing to `ac.transform`.
- QueueGraph carries the condition as an SSA identity into generated policy
  code. The policy returns no candidate when false. For a true condition,
  selected output capacity is checked before Arbitrate publication; output
  backpressure leaves Queue and state effects uncommitted.
- This decision does not yet provide multi-arm CFG joins, conditional
  consume-only proposals, multiple state owners, or multiple selected outputs.

**Verification**
- ACIR lit proves `ready_valid_1x0_table`, zero-result firing/plan generation,
  typed condition retention, and C++20 compilation.
- `consume_only_completion.py` consumes and updates one indexed persistent list
  entry without any sink.
- `state_driven_retire.py` combines an outputless allocator with a zero-input
  guarded retire and proves that a full output Queue preserves the committed
  entry until the result can be accepted.

**Source**
- User direction and circular ROB flow review (2026-09-06): infer input/output
  readiness and atomic checks in MLIR while Python remains serial and typed.

## Decision 0176: one firing may atomically update heterogeneous state owners

**Status:** Accepted and implemented for whole-value scalar/list updates

**Context / Goal**
A circular ROB allocation updates an indexed entry, tail, occupancy, and tag
metadata together. Treating those as independent rules permits partial updates;
packing them into one artificial Python state object hides useful storage
footprints and encourages whole-structure copies.

**Decision (strong constraint)**
- One rule may assign multiple lexical persistent variables. Python continues
  to use ordinary scalar and list assignments; it names no transaction group or
  physical state primitive.
- Storage selection may realize those variables as heterogeneous Tables.
  `ACDataFlowAnalyzer` records every ordered owner/index/field footprint, and
  effect inference retains one state effect per distinct owner.
- QueueGraph stores an ordered `state_writes` list. Each write contains the
  selected owner, index/value SSA identities, mode, and field footprint. The
  first slice permits one whole-value proposal per owner.
- `gfsim::StateTransitionPlan` carries one allocation-free optional write per
  heterogeneous owner and selected optional outputs. `QueueStateTransition`
  computes one candidate during Work,
  then prepares and publishes all state owners and Queues in stable Arbitrate
  order using one commit-group identity.
- Failure to reserve any owner cancels every prior reservation. Publication is
  no-fail after the complete prepared-set check; Xfer exposes all committed
  updates together.
- Existing one-owner rules keep the smaller `QueueTableTransition` template.
  Multi-owner code generation selects `QueueStateTransition` without cloning
  Python rule mechanics into the backend.

**Verification**
- Runtime tests prove cursor, indexed entry, input consumption, and output
  production remain unchanged under backpressure and commit together once all
  resources are ready.
- MLIR lit proves two heterogeneous owners survive rule lowering as structured
  state writes and generate compiling C++20 `QueueStateTransition` code.
- `multi_state_allocate.py` proves a scalar tail and persistent Python list are
  updated together through the complete frontend-to-gfsim path.

**Source**
- Circular ROB implementation review (2026-09-06): preserve Pythonic state
  decomposition without sacrificing atomic multi-resource commit.

## Decision 0177: the variable/rule flow implements a real circular ROB

**Status:** Accepted and implemented for a four-entry generated gfsim model

**Context / Goal**
The rule and variable flow must be validated by a real state machine rather
than a FIFO or `reorder` wrapper. The acceptance model must expose the failure
modes that motivated compiler-inferred Queue checks and atomic state effects.

**Decision (strong constraint)**
- `circular_rob.py` uses ordinary typed system parameters/returns, `@ac.rule`,
  scalar variables, one fixed Python list, record field expressions, and a
  single guarded block. It contains no explicit source/sink, Queue/Table/Reg,
  ready/full, pop/push, reservation, publish, or commit spelling.
- Persistent state consists of fixed-width head/tail indices, occupancy, a
  recovery epoch, and four entries. Each entry carries its slot index,
  per-reuse generation, recovery epoch, value, and done bit.
- Generation and recovery epoch are 16-bit finite tags. The environment must
  not retain a completion across `2^16` reuses of the same slot or `2^16`
  recoveries; violating that bound is outside this ROB's anti-ABA contract.
- Recovery has highest lexical priority and atomically moves head to tail,
  clears occupancy, and advances epoch. Old entries need not be physically
  cleared because completion validates both the entry tag and current epoch.
- Allocation stalls at occupancy four. A successful allocation reads the old
  slot generation, writes the entry, advances the wrapping tail, increments
  occupancy, and publishes the allocated tag/value in one transaction.
- Completion is consume-only. It always consumes its input; a stale generation
  or epoch produces a committed no-op replacement and cannot mark the current
  entry done.
- Retirement is zero-input and state-driven. It selects only the current head
  when occupancy is non-zero and done, clears that entry, advances the wrapping
  head, decrements occupancy, and publishes the value atomically. Output
  backpressure preserves head, occupancy, and entry state.
- Allocation and retirement output capacity, external input availability,
  owner conflicts, and commit mechanics are inferred below Python.

**Verification**
- The generated-model acceptance test fills the ROB, proves the fifth request
  is retained, completes entries out of order, and observes in-order retirement.
- It holds both allocation and retirement outputs full and proves no associated
  state update occurs.
- It performs more than four allocations/retirements to prove index wrap,
  reuses slot zero with a new generation, consumes an old completion as a no-op,
  flushes an uncompleted entry, rejects its old-epoch completion, and retires a
  new-epoch allocation.
- Frozen ACIR contains four stateful firings and generated C++ uses shared
  `QueueStateTransition`/`QueueTableTransition` templates with touched-entry
  Table commits.

**Source**
- User objective (2026-09-05 through 2026-09-06): complete the compiler-driven
  flow until it can express and efficiently simulate a real ROB/ISQ.

## Decision 0178: QueueGraph freezes module definitions and specializations before planning

**Status:** Accepted and implemented through the first reusable pure gfsim module

**Context / Goal**
The flat QueueGraph loses the distinction between one reusable module
definition and its placements. Once that information is erased, the gfsim
generator can only emit instance-specific policies and runtime objects, which
duplicates code and prevents specialization-keyed reuse.

**Decision (strong constraint)**
- Module-preserving QueueGraph reuses existing `ac.system`, `ac.module`,
  `ac.instance`, and module-local `ac.scope`; it does not introduce a second
  module vocabulary or expose backend objects in Python.
- `ac.scope` is legal directly in an `ac.module` Graph region only for a
  QueueGraph model. Other structured ACIR models retain their existing Graph
  legality contract.
- The topology-freeze pass computes one SHA-256 definition fingerprint for
  every materialized module. It computes each instance specialization from
  that definition fingerprint and its canonical static argument dictionary.
  Repeated instances with the same definition and arguments therefore receive
  the same specialization identity regardless of hierarchy path.
- The selected root receives its own specialization identity. The global seal
  retains the selected system, elaborated instance-owner manifest, and topology
  digest. The verifier recomputes all fingerprints and rejects missing, forged,
  or stale specialization data before QueueGraph planning.
- This first slice permits only materialized modules whose interfaces contain
  Queue values and whose Graph bodies contain scopes, instances, and
  `ac.return`. Extern/generated modules and collection instances remain blocked
  until their specialization and binding contracts are equally closed.
- QueueGraph planning builds definition-, specialization-, and
  instance-indexed records. A reusable body occurs once under its
  specialization record; root instances contain only interface bindings and
  the specialization key, so planning does not copy the body per placement.
- Blocks and instances share one dense lexical order inside each definition.
  Dense runtime IDs and dispatch rows preserve that order even though the plan
  stores blocks and instance bindings in separate typed collections.
- The first gfsim lowering emits one C++ implementation class for a pure
  one-input/one-output transform specialization. Multiple placements construct
  that same class with distinct Queue bindings and runtime object IDs; the
  generated root dispatch table references each instance without duplicating
  the policy or class body.
- Stateful multi-rule modules, nested instances, arbitrary Queue arity, and
  per-instance persistent state remain the next lowering slice. It may not
  flatten repeated instances back into the root.

**Verification**
- ACIR lit freezes a root with two instances of one Queue transform module,
  proves the instance specialization fingerprints are byte-identical, and
  proves a second freeze is byte-identical.
- Canonical QueueGraph JSON contains one `Increment` specialization body and
  two instance binding records that reference its identical fingerprint.
- Native CodeGen executes both generated instances, proves each receives and
  transforms its own Queue value, and asserts that the C++ source contains one
  implementation class with two members of that class.
- The complete ACIR lit suite remains green, including the existing rejection
  of mixed flat/structured QueueGraph input.

**Source**
- User direction (2026-09-05 through 2026-09-06): keep Python serial and
  parameterized, infer structure in MLIR, and make the backend reuse the same
  module implementation instead of expanding every instance.

## Decision 0179: specialization reuse shares code but never persistent state

**Status:** Accepted and implemented for one stateful firing and one Table

**Context / Goal**
Generating one class per specialization is insufficient if repeated instances
also alias one Table object or one transaction identity. A ROB/ISQ may reuse a
module implementation many times, but every instance must retain independent
committed state, reservations, Queue bindings, and runtime IDs.

**Decision (strong constraint)**
- A module-local lexical `ac.scope` is the ownership and lookup domain for
  state declared in that scope. Table lookup walks lexical ancestor blocks; it
  does not turn `ac.scope` or `ac.module` into an MLIR SymbolTable that would
  shadow unrelated memory, process, or module references.
- Table stable IDs are definition-local. Two different reusable definitions
  may use the same local state name without being classified as duplicate
  state; duplicate IDs inside one definition remain illegal.
- A stateful specialization plan stores the Table and firing body once. Its
  generated C++ class declares the Table and `QueueTableTransition` members
  once as class layout.
- Every placement constructs a distinct object of that class with its own
  Table instance, Queue references, firing ID, and Table ID. Dispatch expansion
  assigns a dense ID range to each placement while retaining placement lexical
  order.
- The first slice covers one input, one output, one firing, and one Table.
  Multi-rule/multi-owner modules, arbitrary Queue arity, and nested instances
  must extend this ownership structure rather than returning to flattening.

**Verification**
- Structured ACIR freezes and plans two instances of one stateful
  `Accumulator` specialization with one stored module body.
- Generated C++ contains one `Accumulator` implementation class and two
  members of that class. Each member constructs its own `SimTable` and receives
  its own dense firing/Table object IDs.
- Runtime execution sends `1,2` to the left instance and `10` to the right.
  Results are `1,3` and `10`, proving code reuse with independent persistent
  state.

**Source**
- User direction (2026-09-05 through 2026-09-06): preserve modules in the
  efficient gfsim backend while compiler analysis owns state and transaction
  mechanics.

## Decision 0180: reusable modules preserve multi-rule Queue arity and lexical arbitration

**Status:** Accepted and implemented for multiple single-owner firing rules

**Context / Goal**
A ROB/ISQ module has multiple request, completion, recovery, and result ports.
Restricting a reusable module to one input, one output, and one firing would
force authors to split one state owner across artificial modules or return to a
flattened root graph.

**Decision (strong constraint)**
- A stateful specialization may expose multiple typed Queue inputs and outputs.
  Each firing binds the exact input/output subset it uses; unused module ports
  do not create dummy transaction participants.
- Multiple firing blocks may share one module-local Table. Their frozen
  `ac.rule_priority` values remain strictly ordered, and generated runtime IDs
  plus dispatch rows preserve the same lexical order inside every instance.
- Every firing retains its own policy and `QueueTableTransition` member in the
  single specialization class. The class owns one Table member per state owner;
  every placement constructs its own class object and therefore its own state.
- When same-instance firings conflict, the lower lexical priority publishes in
  Arbitrate. The losing firing consumes no input and must recompute from the new
  committed state on a later activation. Firings in different instances never
  conflict merely because their classes are identical.
- The first slice requires firing inputs/outputs to bind module interface
  Queues directly and supports one state owner. Internal Queue pipelines,
  multiple state owners, and nested reusable instances remain follow-up work.

**Verification**
- `DualAccumulator` has two inputs, two outputs, two firing rules, and one
  shared Table; two root placements reference one specialization body.
- In the left instance both inputs are ready together. Rule A commits `1`;
  rule B retains input `2`, observes the new state, and later commits `3`.
- In the right instance only rule B receives `10` and independently commits
  `10`. Generated source contains one implementation class and two objects of
  that class.

**Source**
- User direction (2026-09-05 through 2026-09-06): modules may have many input
  and output queues, while readiness, backpressure, atomicity, and arbitration
  are inferred and inserted below Python.

## Decision 0181: reusable module instances own atomic multi-owner transition groups

**Status:** Accepted and implemented for one multi-owner firing

**Context / Goal**
ROB allocation and recovery update several persistent variables together. A
reusable module must preserve that transaction boundary without sharing state
between placements or generating one copy of the policy per instance.

**Decision (strong constraint)**
- One firing specialization may carry multiple ordered `state_writes`. Every
  referenced Table must belong to the firing's module-local lexical scope.
- The specialization plan stores each owner type, index/value identity, and
  field footprint once. Generated C++ emits one `StateTransitionPlan` policy,
  one merge policy per owner, and one `QueueStateTransition` member in the
  specialization class.
- Each class instance constructs a distinct Table object for every owner and a
  distinct dense ID range for the firing and those Tables. Reusing the class
  never aliases committed state, reservations, or commit-group identity.
- Work computes one immutable multi-owner candidate. Arbitrate reserves every
  selected Queue and Table owner before publishing any effect. Xfer exposes all
  writes and Queue transfers together.
- This slice supports one multi-owner firing per reusable specialization.
  Combining multiple rules with multiple owners and nesting specializations
  remains follow-up work.

**Verification**
- `StatePair` updates independent cursor and total Tables and reports a value
  derived from both newly proposed states.
- Two placements share one generated class. Left inputs `3,5` report `4,10`;
  the independent right input `10` reports `11`, proving that both owners
  advance atomically per instance without cross-instance state sharing.
- Generated source uses `QueueStateTransition` and contains two Table members
  in the single specialization class.

**Source**
- User direction (2026-09-05 through 2026-09-06): infer multi-variable atomic
  work in MLIR and reuse backend module implementations without flattening or
  sharing runtime state.

## Decision 0182: reusable modules combine multi-rule and multi-owner semantics

**Status:** Accepted and implemented for direct interface Queue bindings

**Context / Goal**
A real ROB combines several rules with several persistent variables. Supporting
multi-rule and multi-owner modules only as disjoint backend modes would still
prevent one reusable ROB specialization from containing allocation, recovery,
completion, and retirement together.

**Decision (strong constraint)**
- One specialization class owns the union of its module-local Table
  declarations. Every placement constructs that complete owner set exactly
  once with an independent dense ID range.
- Each firing preserves its own ordered owner subset from `state_writes` and
  generates one `QueueStateTransition`. Owners not referenced by that firing
  are not reserved and do not participate in its commit group.
- Each firing also binds only its selected input/output Queue subset. Rule
  policies, merge policies, and runtime members are emitted once per
  specialization, never once per placement.
- Runtime IDs and dispatch rows preserve firing lexical order before the
  instance-local Table rows. Conflicting rules retain losing inputs and
  recompute all proposed owner values from the next committed snapshot.
- The current slice uses direct module interface Queue bindings. Nested module
  instances and internal Queue graphs remain follow-up work.
- The structured generator now has several proven stateful shapes. Before
  adding further shapes, those paths should be consolidated behind one typed
  stateful-specialization emitter without changing behavior.

**Verification**
- `DualState` exposes two inputs and two outputs, contains two rules, and owns
  cursor and total Tables. Both rules atomically update both owners.
- With both left inputs ready, rule A reports `2`; rule B retains its input,
  observes cursor/total after A, and reports `5`. The independent right rule B
  reports `11`.
- Generated source contains one `DualState` class, two instance members, two
  Table members in the class, and two `QueueStateTransition` members.

**Source**
- User objective (2026-09-05 through 2026-09-06): complete a reusable,
  compiler-inferred ROB/ISQ flow with many ports, rules, and atomic persistent
  variables.

## Decision 0183: nested specializations preserve class reuse and dense runtime identity

**Status:** Accepted and implemented for direct wrapper modules

**Context / Goal**
Real cores are hierarchical. If a reusable parent module causes its reusable
child body to be flattened or regenerated for every parent placement, module
specialization has merely moved the duplication one level down.

**Decision (strong constraint)**
- QueueGraph planning constructs specialization dependencies in deterministic
  child-before-parent topological order. Cyclic or incomplete specialization
  graphs fail before code generation.
- A parent specialization plan contains its own instance bindings and one plan
  for each directly referenced child specialization. Multiple parent instances
  reference the same parent plan and do not copy child bodies.
- Generated C++ emits each reachable specialization class after its children
  and before its parents. A direct wrapper class owns child module objects but
  no duplicated child policy or implementation body.
- Runtime object counts are computed bottom-up. Each parent instance receives
  one dense ID interval and partitions it deterministically across child
  instances; `dispatch_row(index)` delegates into the corresponding child
  interval.
- The first slice supports direct wrappers whose Queues pass from the parent
  interface to child interfaces. Local blocks combined with nested instances
  and internal Queue storage remain follow-up work.

**Verification**
- `Wrapper` instantiates one `Increment` specialization. The root instantiates
  `Wrapper` twice.
- Canonical plan JSON contains one Wrapper plan with one nested Increment plan.
  Generated C++ contains exactly one class for each specialization and two root
  Wrapper members.
- Runtime inputs `5` and `10` independently produce `6` and `11` through the
  nested hierarchy.

**Source**
- User direction (2026-09-05 through 2026-09-06): preserve and call reusable
  modules in the efficient backend instead of expanding hierarchy per instance.

## Decision 0184: parent specializations own internal Queue storage

**Status:** Accepted and implemented for one local transform feeding one child

**Context / Goal**
A useful parent module does more than forward interface Queues. It may preprocess
requests, buffer them internally, and then invoke a reusable child. Flattening
that internal Queue into the root would couple repeated parent instances and
erase the module's storage boundary.

**Decision (strong constraint)**
- QueueGraph distinguishes exported result Queues from internal Queues in every
  specialization plan. Internal Queues contribute to the specialization's
  bottom-up runtime object count.
- Each parent instance constructs its own internal `SimQueue` objects. Their
  dense IDs precede local block and child-object IDs inside the parent's
  recursively assigned interval.
- Local blocks and child instance bindings refer to the same parent-local Queue
  object. Child results that are returned directly bind the parent's external
  output Queue and need no duplicate internal storage.
- Codegen emits local policy/block members and nested child members once in the
  parent specialization class. Repeating the parent constructs those members
  and Queues again but does not duplicate either class body.
- The first slice supports one pure local transform followed by direct child
  instances. Arbitrary internal graphs, multiple local blocks, and local state
  combined with children remain follow-up work.

**Verification**
- `PrepareAndIncrement` locally increments into an internal Queue, then invokes
  the reusable `Increment` child. The root instantiates the parent twice.
- Canonical plan preserves the local transform, internal `prepared` Queue, and
  child binding. Generated source contains one internal Queue member in the
  parent class and one class per specialization.
- Inputs `5` and `10` independently traverse both stages and produce `7` and
  `12`.

**Source**
- User direction (2026-09-05 through 2026-09-06): preserve modules and reuse
  implementation code while every module instance owns its own Queue/state
  resources.

## Decision 0185: Python module calls lower to structured QueueGraph specializations

**Status:** Accepted and implemented for pure typed 1x1 modules

**Context / Goal**
The hierarchy-preserving compiler path is not useful if Python authors must
write `ac.instance`, Queue ports, specialization keys, or internal source/sink
objects. Reuse must follow from ordinary typed function definitions and calls.

**Decision (strong constraint)**
- `@ac.module` declares a reusable Python function. Its parameters and return
  annotation describe values, not Queue hardware ports.
- `@ac.system` invokes a module with an ordinary assignment call. Python does
  not name an instance object, specialization fingerprint, source, sink,
  readiness, backpressure, or atomic transaction mechanics.
- The first frontend slice accepts one typed positional argument, one typed
  result, and one pure expression return. It lowers the expression to a
  module-local transform, system parameters/results to internal boundaries, and
  calls to structural `ac.instance` operations.
- Topology freeze computes definition and specialization fingerprints. Repeated
  calls to the same definition and static argument set generate one C++
  implementation class and multiple independently bound objects.
- The native MLIR tool path is required for `@ac.module`; the old direct-Python
  C++ shortcut remains only for supported flat models.
- Stateful or nested Python modules, arbitrary arity, static parameters, and
  repeated-value fanout remain follow-up work. They must extend ordinary calls,
  not add marker or hardware resource syntax.

**Verification**
- `inferred_module_pipeline.py` defines `increment(value: u8) -> u8` as
  `value + 1` and calls it twice from a typed system.
- Frontend tests prove structured `ac.system`, `ac.module`, and two
  `ac.instance` operations are inferred with no public source/sink or Queue
  object.
- The native tool flow freezes, plans, generates, compiles, and executes one
  reused implementation class; inputs `5,10` produce `6,11`.

**Source**
- User direction (2026-09-05 through 2026-09-06): keep Python simple,
  parameterized, serial, and variable-oriented while MLIR infers module and
  transaction structure.

## Decision 0186: nested Python calls infer parent and child specializations

**Status:** Accepted and implemented for one direct nested call

**Context / Goal**
Python module reuse must compose. Requiring authors to switch to explicit
instance or Queue syntax when one module calls another would leak the compiler's
hierarchy representation back into the frontend.

**Decision (strong constraint)**
- A pure typed `@ac.module` expression return may be a direct call to another
  typed module using its ordinary parameter value.
- The frontend verifies the child signature and emits a parent `ac.module` with
  one child `ac.instance`. Python names neither the instance nor its
  specialization.
- The system invokes the parent with ordinary assignments. Topology freeze,
  QueueGraph planning, and gfsim codegen derive child-before-parent
  specialization identity, class order, and dense runtime-ID partitions.
- The first slice supports one direct nested call. Multi-statement internal
  module graphs, stateful nested modules, arbitrary arity, recursion, and static
  parameters remain follow-up work.

**Verification**
- `wrapper(value: u8) -> u8` returns `increment(value)`; the typed system calls
  `wrapper` twice.
- Frontend tests prove the parent contains one inferred child instance. The
  canonical plan contains one Wrapper specialization with one nested Increment
  specialization.
- Native integration emits one class for each definition and two root Wrapper
  objects; inputs `5,10` produce `6,11`.

**Source**
- User direction (2026-09-05 through 2026-09-06): keep module composition
  Pythonic and infer reusable backend hierarchy in MLIR.

## Decision 0187: lexical Python variables infer reusable stateful modules

**Status:** Accepted and implemented for one scalar state variable

**Context / Goal**
A stateful module should still look like serial Python. Authors must not declare
Queue ports, registers, Tables, source/sink nodes, readiness, backpressure, or
transaction mechanics merely because a local value persists across firings.

**Decision (strong constraint)**
- `ac.var` remains the only variable family; no parallel long-form alias or IR
  concept is introduced. A normal annotated variable declared inside a module
  body acquires that module's lexical ownership.
- `ac.var.decl` carries a stable lexical identity without becoming a generic
  MLIR symbol. AC-specific resolution searches enclosing lexical regions, like
  existing Table resolution, so nested state does not alter unrelated symbol
  visibility. The frontend emits `ac.var.read` and `ac.var.assign` in one
  transient rule; it does not choose concrete storage.
- The first frontend slice accepts one zero-initialized scalar state variable,
  one assignment, and a return of that updated variable. Module parameters and
  results remain ordinary typed Python values.
- Rule lowering runs storage selection before effect analysis. The public
  compiler analysis surface is `ACDataFlowAnalyzer`; the generic MLIR
  dataflow solver remains hidden inside its private implementation.
- MLIR selects a one-entry Table, derives input consumption, output capacity,
  state footprints, lexical arbitration, and one Queue-plus-state atomic
  transaction. Topology freeze derives the specialization fingerprint.
- Repeated calls emit one gfsim implementation class. Every placement owns an
  independent Table instance and therefore independent persistent state.
- Multiple state variables and direct module arity are addressed by Decisions
  0188 and 0189. Conditional updates, static parameters, and repeated-value
  fanout remain follow-up work.

**Verification**
- `inferred_stateful_module.py` defines only a typed `total` local, ordinary
  assignment, and return; the raw ACIR contains only `ac.var` state operations.
- The focused MLIR regression proves lexical symbol resolution, storage
  selection, rule lowering, topology freeze, specialization planning, and C++
  compilation.
- The frontend suite passes 74/74 tests. Native integration emits one reused
  accumulator class and two independent instances: left inputs `1,2` produce
  `1,3`, while right input `10` produces `10`.

**Source**
- User direction (2026-09-06): keep `ac.var` as the sole Pythonic variable
  concept, expose `ACDataFlowAnalyzer` as the compiler analysis, and infer all
  state/Queue transaction mechanics in MLIR.

## Decision 0188: multiple lexical module variables form one atomic transaction

**Status:** Accepted and implemented for scalar state in a 1x1 module

**Context / Goal**
A useful ROB/ISQ module owns several correlated values such as head, tail,
occupancy, epoch, and entries. Supporting only one Python variable per module
would force authors to package unrelated state manually or leak backend
resource concepts into the frontend.

**Decision (strong constraint)**
- A stateful `@ac.module` may declare multiple zero-initialized scalar lexical
  variables before its serial work statements. Each declaration remains in the
  `ac.var` family and derives ownership from the module body scope.
- The first multi-state slice requires exactly one assignment to every declared
  state. All committed values are read once at activation start. Python
  assignments update the local immutable snapshot environment in source order,
  so later expressions observe earlier proposed values without committing them.
- The frontend emits one transient rule containing every `ac.var.read` and
  `ac.var.assign`. It does not emit Table types, readiness, full/empty checks,
  reservations, or commit operations.
- Storage selection creates one owner per lexical variable.
  `ACDataFlowAnalyzer` records their ordered footprints, rule lowering derives
  input/output checks, and QueueGraph preserves every state write.
- gfsim uses one `QueueStateTransition` to prepare, publish, and commit all
  selected owners plus input consumption and output production together.
  Output backpressure leaves every owner and input unchanged.
- Repeated calls still generate one specialization class. Each placement
  constructs its complete owner set independently.
- This slice remains one-input/one-output with unconditional assignments;
  Decision 0189 adds direct-interface module arity. Conditional updates, CFG
  joins, shaped module state, static parameters, and inferred fanout remain
  follow-up work.

**Verification**
- `inferred_multi_state_module.py` declares `count` and `total`, updates both in
  serial Python, and returns `total + count` without naming any hardware
  resource or transaction primitive.
- Raw ACIR contains two declarations, reads, and assignments in one rule. The
  canonical plan contains two state writes and generated C++ uses one
  multi-owner `QueueStateTransition` in one reused `Tally` class.
- With sinks paused, the second left input remains committed at the input
  boundary while the first output applies backpressure. After release, left
  inputs `1,2` produce `2,5`; the independent right input `10` produces `11`.

**Source**
- User objective (2026-09-06): continue completing the compiler-inferred flow
  until a practical ROB/ISQ can be authored as simple parameterized serial
  Python and executed efficiently without frontend transaction mechanics.

## Decision 0189: systems and reusable rule modules share one body lowering

**Status:** Accepted and implemented for direct-interface rule graphs

**Context / Goal**
The existing circular ROB already exercised four rules and five state owners,
but it was a root system. Reimplementing every rule, state, arity, and control
feature in a separate module-shape recognizer would make the Python language
and MLIR semantics diverge before the ROB could become reusable.

**Decision (strong constraint)**
- `parse_queue_program` accepts either a system or module callable entry while
  using the same rule definitions, lexical variable parsing, typed expression
  lowering, state-write bindings, guards, and source-order rule semantics.
- `lower_queue_program` uses the same QueueProgram event renderer for both
  callables. Only the boundary policy differs: a system materializes internal
  source/sink nodes, while a module binds its typed arguments to borrowed Queue
  values and yields its typed returned Queues through `ac.return`.
- A rule-backed module may have arbitrary typed input and output interface
  arity and multiple ordinary `@ac.rule` calls. Individual rules retain the
  verified zero-or-one-output contract; module output arity is the union of
  distinct internal rule results, not one multi-output firing.
- Module-local lexical variables are rendered inside one body scope, then
  storage-selected and analyzed by the existing MLIR rule pipeline. Python
  still declares no Queue port, source/sink, ready/full test, pop/push, Table,
  reservation, or commit group.
- Ordinary tuple assignment places a multi-result module. Topology freeze and
  QueueGraph derive interface bindings and a specialization key; codegen emits
  one class per specialization and constructs independent state per placement.
- The reusable circular ROB module has three typed inputs, two typed outputs,
  five lexical owners, and four rules: recover, allocate, complete, and retire.
  The root places it twice without copying its body.
- This slice supports direct interface-to-rule graphs. Arbitrary internal Queue
  graphs, repeated-input fanout inside a specialization, static parameters,
  general CFG joins, and incremental activation remain follow-up work.

**Verification**
- Frozen ACIR contains one `rob` definition with four firings and two instance
  placements. Its canonical specialization plan records 3 inputs, 2 outputs,
  5 Tables, and 4 firing blocks.
- Generated C++ contains one `Rob_<fingerprint>` class, two instance members,
  and multi-owner `QueueStateTransition` objects; it compiles as C++20.
- Both placements allocate index zero independently, complete and retire their
  own values `100` and `200`, and a later left-only allocation advances only
  the left instance to index one.
- The original single-instance circular ROB regression continues to cover
  output backpressure, full/empty distinction, wrap, stale generation,
  recovery epoch, out-of-order completion, and in-order retirement.

**Source**
- User objective (2026-09-06): continuously inspect and complete the flow until
  a real ROB/ISQ is expressible as simple parameterized Python and runs through
  compiler-inferred atomic simulation.

## Decision 0190: QueueGraph materializes direct-module incremental activation

**Status:** Accepted and implemented for direct leaf specializations

**Context / Goal**
The reusable ROB has 32 runtime objects. Calling every dispatch row every tick
would preserve correctness but scale linearly with all 200+ model nodes even
when only one input or owner changes. gfsim already has a dense dispatch table
and CSR activation scheduler, but QueueGraph did not produce its adjacency.

**Decision (strong constraint)**
- QueueGraphPlan stores compiler-derived activation nodes, exact directed
  edges, and an initial frontier. Nodes distinguish module interface inputs and
  outputs, owned Queues, firing blocks, and Tables by deterministic local
  ordinal; specialization plans never store placement-specific ObjectIds.
- For each runtime block, the compiler derives its resource set from typed
  Queue inputs/outputs, Table state writes, and Table references in expression
  plans. A committed resource wakes the complete transaction closure: the
  target block plus every Queue/Table that must participate in that epoch's
  Work/Arbitrate/Probe/Commit barrier.
- Every read or write owner is a wake source. This ensures a lower-priority
  rule that lost Table arbitration retries after the winning owner commits.
- Zero-input rules contribute their closure to the initial frontier and are
  subsequently woken by referenced Table changes or output Queue dequeue.
- Extracted plans materialize canonical sorted/deduplicated edges. The plan
  verifier independently re-derives them and rejects missing, extra, or
  reordered activation evidence.
- Structured codegen binds interface nodes to caller Queue IDs and local
  block/Table nodes to each placement's dense object-ID interval. It emits CSR
  `activation_offsets()`, `activation_targets()`, and `initial_work_ids()`.
- `activation_complete()` is true only when every reachable placement is a
  directly bindable leaf specialization without unsupported internal/nested
  Queue activation. Partial adjacency is never advertised as complete.
- A zero-input Queue/Table transition does not report itself perpetually
  runnable merely because its empty input tuple is vacuously ready. Initial and
  resource-driven activation own its execution instead.
- The first slice requires the caller to schedule an externally proposed Queue
  in `SimSystem`. Automatic external offer/dequeue adapters, nested/internal
  specialization binding, distinct Work/Xfer closure CSR, and semantic-change
  filtering remain required follow-up work.

**Verification**
- The reusable ROB plan records root and specialization activation evidence;
  generated C++ reports a complete physical plan for both direct placements.
- A forged plan with one removed edge fails verification before codegen.
- One executable runs the reusable ROB once with the full-scan reference and
  once with `SimSystem` incremental activation. Both instances allocate,
  complete, and retire identical `100/200` results.
- The activated model completes normally rather than reporting zero-input
  no-progress. Its 32-row graph executes 134 scheduled Work calls versus 256
  calls for an eight-tick full scan; activation traverses 306 conservative
  closure edges.

**Source**
- User objective (2026-09-06): keep gfsim efficient for large DavinciOO-scale
  graphs while deriving readiness, backpressure, state wakeups, and atomic
  transaction mechanics below the Python frontend.

## Decision 0191: MLIR freezes typed rule activation and transaction resources

**Status:** Accepted and implemented for rule-backed blocks

**Context / Goal**
Decision 0190 proved that QueueGraph activation can execute the reusable ROB,
but its first plan inference still reconstructed rule resources from extracted
Queue names and expression/Table references. Activation and atomic closure are
rule semantics and must be frozen before backend planning.

**Decision (strong constraint)**
- ACIR defines the closed enum `ActivationResourceKind` with input Queue,
  output Queue, and state cases. Resource records use an ordinal for Queue
  endpoints and a symbol reference for state owners.
- The dedicated `ac-infer-rule-activation` MLIR pass runs after effect and
  footprint analysis. It derives `ac.rule.activation_sources`,
  `ac.rule.transaction_resources`, and `ac.rule.initially_active` from typed
  rule operands/results, `ACDataFlowAnalyzer` footprints, and Table proposals.
- Activation sources include every input/output Queue and every state owner
  read or written. Transaction resources include consumed inputs, possibly
  produced outputs, and every writable state owner. Zero-input rules are
  initially active.
- Rule-to-firing lowering preserves the evidence. Pure firing canonicalization
  preserves it on proof-carrying transforms. ACIR verification independently
  reconstructs the exact resource records and rejects partial, reordered, or
  forged evidence.
- QueueGraph blocks retain the typed resource records and initial flag. Physical
  activation inference resolves those records to specialization-local nodes;
  only structural non-rule blocks use topology fallback.
- Generated root models provide `offer_<input>(SimSystem&, value)` adapters.
  Each adapter schedules the input Queue at the current epoch before proposing
  the value, so callers no longer manually coordinate Queue IDs with the
  scheduler. Generated initial-frontier scheduling is similarly centralized.
- Nested/internal specialization binding, separate Work/Xfer closure CSR,
  sink-free output dequeue adapters, and semantic-change wake filtering remain
  follow-up work.

**Verification**
- Lowered multi-state and zero-input rule MLIR prints enum-typed activation and
  transaction records; QueueGraph JSON marks those blocks as carrying MLIR
  evidence and preserves the initial flag.
- Replacing a zero-input rule's `initially_active=true` with false is rejected
  directly by the ACIR firing verifier.
- The reusable ROB incremental harness uses generated offer and initial-work
  adapters, with no manual external Queue scheduling, and remains result-equal
  to the scan reference.
- Focused rule-lowering lit, QueueGraph plan/codegen, and reusable ROB runtime
  tests pass.

**Source**
- User direction (2026-09-06): insert readiness, backpressure, sink, and atomic
  transaction behavior through MLIR dataflow analysis rather than expressing
  those mechanics in the Python frontend.

## Decision 0192: activation binding recurses through specialization hierarchy

**Status:** Accepted and implemented for supported nested/internal shapes

**Context / Goal**
Marking activation incomplete for every nested wrapper would force hierarchical
models back to full dispatch scans. Activation must follow instance bindings
without flattening or generating a class per placement.

**Decision (strong constraint)**
- Physical activation materialization recursively walks specialization plans.
  Each call receives the placement's contiguous ObjectId interval and a map
  from the child's logical interface names to the parent's actual Queue IDs.
- Local runtime layout is deterministic: internal Queues, local blocks, local
  Tables, then child intervals. The activation resolver uses the same layout as
  specialization constructors and `dispatch_row(index)`.
- Interface input/output nodes resolve to borrowed parent Queues. Internal Queue
  nodes resolve within the parent's interval. Block and Table nodes resolve to
  their local offsets. Child bindings are then constructed from the parent's
  resolved Queues and the child's sub-interval.
- Edges from all reachable children are merged, sorted, and deduplicated into
  one root CSR plan. Recursive binding never copies or modifies a generated
  specialization class.
- `activation_complete()` is true for every hierarchy shape currently admitted
  by structured QueueGraph codegen: direct leaf transforms/stateful modules,
  direct wrappers, and the supported local-transform plus internal-Queue child
  shape.
- General arbitrary internal graphs remain limited by the structured codegen
  shape contract itself, not by activation identity binding.

**Verification**
- Direct and nested Python module integrations run both scan and incremental
  activation paths and produce the same `6/11` results.
- The mixed `PrepareAndIncrement` specialization resolves its internal Queue,
  local block, and nested child interval and emits
  `activation_complete() == true` while preserving one class per
  specialization.
- Focused nested and mixed QueueGraph plan/codegen tests pass, and generated C++
  remains valid.

**Source**
- User objective (2026-09-06): retain backend module reuse and per-instance
  state while scaling activation to the hierarchical module graph.

## Decision 0193: Work activation and same-epoch Xfer closure are separate

**Status:** Accepted and implemented for generated QueueGraph activation

**Context / Goal**
Decision 0190 initially expanded every wake target into a block plus all of its
Queue/Table resources. This was correct but called no-op `doWork` on resources
and conflated next-epoch computation with same-epoch commit participation.

**Decision (strong constraint)**
- QueueGraph stores two independently verified edge sets:
  `activationEdges` maps a committed resource to a next-epoch Work block;
  `workClosureEdges` maps that block to Queue/Table resources that must join its
  current-epoch atomic barrier. Initial activation contains Work blocks only.
- Generated code recursively binds both edge sets and emits separate canonical
  CSR arrays for activation and Work closure.
- `SimSystem` drains scheduled Work and explicit external Xfer frontiers
  separately. It invokes `doWork` only for scheduled Work IDs, then forms the
  Xfer closure from those workers plus externally enrolled resources.
- Arbitration runs owners/workers first in stable ID order, then closure-only
  Queue/Table resources. This is required because a state transition publishes
  its resource reservations during owner arbitration.
- Probe runs over the entire closure before any member commits. Only after all
  pending states and once-per-tick constraints validate does the second loop
  perform Commit, preserving a global no-fail barrier.
- `scheduleExternalXfer(id)` accepts only dispatched Queue/EventQueue resources
  outside a frozen Work epoch. Generated `offer_<input>` checks capacity,
  enrolls that Queue for Xfer, and proposes the value; an external Queue no
  longer appears in Work counts.
- Work-closure traversal has a separate counter. Activation traversal continues
  to count cross-epoch wake edges.
- Compiler-owned root output boundaries and symmetric external dequeue adapters
  remain follow-up work. Current typed system returns still use compiler-
  generated sink nodes.

**Verification**
- Plan verification independently rejects both a removed activation edge and a
  removed Work-closure edge.
- A runtime spy proves the closure resource commits without receiving
  `doWork`; a separate test proves an externally enrolled Queue commits with
  zero Work invocations.
- Direct, nested, mixed-internal, and reusable ROB activation tests pass with
  the two CSR plans.
- For the 32-row dual-instance ROB scenario, Work falls from the initial
  conservative activation's 134 calls to 40 calls, versus 256 calls for an
  eight-tick full scan. The run traverses 74 wake edges and 150 closure edges
  and produces the same `100/200` results.

**Source**
- User objective (2026-09-06): make ACIR-to-gfsim simulation efficient without
  weakening the compiler-inferred Queue/state atomic transaction contract.

## Decision 0194: host result mode preserves root Queue backpressure

**Status:** Accepted and implemented for structured module systems

**Context / Goal**
Compiler-generated sinks are convenient for standalone tests, but they consume
every system result automatically. A host-integrated simulator needs the root
result Queue to remain full until the host explicitly accepts it; otherwise
output backpressure cannot be exercised as a real boundary condition.

**Decision (strong constraint)**
- `--host-results` is a compiler target option, not a Python type, marker, or
  sink operation. The same typed `@ac.system` return selects either standalone
  internal sinks or host-owned result Queues.
- In host mode the generated Top module has Queue results and returns the exact
  internal result values with `ac.return`. It does not materialize an
  `ac.scope @outputs` or `ac.sink`.
- Structured QueueGraph root modules still cannot borrow Queue inputs; their
  results may be exact Queue types. Freeze fingerprints and root specialization
  identity include this interface.
- QueueGraph records root `interface_outputs`, counts them as consumers for
  static topology verification, and retains their Queue objects in the root
  runtime.
- Generated roots expose read-only `result_N()` Queue access and
  `try_take_result_N(system)`. The latter checks committed availability,
  enrolls the Queue in the external-Xfer frontier, and proposes one pop. Its
  returned value becomes host-accepted only when the next system step commits.
- The Queue dequeue commit follows normal activation edges and wakes every
  producer blocked by that output. No special backend retry or direct state
  mutation is permitted.
- Default standalone generation retains compiler-inserted sinks. Host results
  currently require the structured module flow; unsupported flat QueueGraph
  use fails explicitly.

**Verification**
- Frontend host mode emits a result-bearing Top and no `ac.sink`; native freeze,
  planning, codegen, and C++ syntax gates cover a one-result module.
- The reusable ROB host plan has four root interface outputs and no sink block.
- With the first allocation output held committed, a second request remains in
  the left allocation input Queue. After `try_take_result_0()` commits the
  dequeue, the producer retries and emits the second allocation at slot one.
- The observed values are `100` at slot zero followed by `300` at slot one;
  neither input consumption nor head/tail/count/entry state advances while the
  host output is full.

**Source**
- User direction (2026-09-06): infer output backpressure below Python and keep
  the frontend limited to typed parameters, return values, variables, and
  serial rules.

## Decision 0195: Table activation follows semantic value changes

**Status:** Accepted and implemented for equality-comparable Table entries

**Context / Goal**
Incremental activation must preserve the exact transaction semantics of a full
scan without waking unrelated rules after a committed Table replacement that
leaves the final value unchanged. Stale ROB completions and repeated ISQ
wakeups are common examples: they may consume an input and complete an atomic
transaction while producing no new state for Table subscribers to observe.

**Decision (strong constraint)**
- Commit participation and semantic change are separate runtime facts.
  `hasPendingCommit()` continues to control the complete Probe/Commit barrier;
  it is never weakened by an equality check.
- After a successful commit, each runtime object reports whether observable
  committed state changed. The default remains conservative so existing or
  non-comparable objects wake exactly as before.
- `SimTable<Entry>` snapshots only the touched committed entries, applies all
  field merges followed by replacements in the existing deterministic order,
  and compares the final touched values with that pre-commit snapshot when
  `Entry` is equality-comparable. It never copies or compares the full Table.
- A Table with no selected indices reports no semantic change. A Table whose
  entry type has no equality relation conservatively reports every published
  write as changed.
- `SimSystem` still records the commit tick, progress, and committed
  observations for an equal-value transaction. Only construction of the
  activation-source frontier uses the semantic-change result.
- QueueGraph and MLIR activation evidence are unchanged. They describe the
  static may-depend graph; the runtime semantic-change bit filters dynamic
  propagation without guessing or deleting compiler-derived edges.

**Verification**
- A runtime regression drives the same `QueueTableTransition` twice: an equal
  whole-entry replacement consumes its input and commits without traversing
  the Table activation edge, while a changed replacement traverses the edge
  once and wakes the declared subscriber at the next epoch.
- The complete gfsim suite passes 259/259 tests.
- The reusable dual-instance ROB integration still lowers, links, executes,
  reuses one specialization class, and produces the same allocation and
  retirement results under incremental activation.

**Source**
- User objective (2026-09-06): make compiler-derived activation efficient for
  large QueueGraph models while preserving atomic Queue/state transactions.

## Decision 0196: reusable ROB scan and activation are equivalent at every commit boundary

**Status:** Accepted and implemented for the complete four-entry ROB scenario

**Context / Goal**
Final output equality is insufficient evidence for incremental activation. A
missing wake can preserve one short result sequence while shifting output time,
leaving a Queue token stranded, or changing internal head/tail/count/epoch and
entry state. The reusable ROB therefore needs one reference execution that
compares the entire committed projection at every tick.

**Decision (strong constraint)**
- The equivalence gate constructs two independent objects of the same generated
  reusable dual-ROB model. One schedules every model dispatch row each tick;
  the other installs the generated activation and Work-closure CSR plans and
  schedules only their inferred frontiers.
- A test-only Process clock advances both runtimes through identical epochs. It
  adds no Queue/Table proposal and no committed state. The scan clock schedules
  every model row; the incremental clock schedules only itself.
- After every Xfer boundary, the gate compares all ten root Queue images, both
  instances' head/tail/count/epoch values, all eight resident entries, and the
  complete validated commit timeline.
- The validated profile records `{epoch, ObjectId, semanticChanged}` for every
  successful object commit after the global Probe barrier. Commit/progress
  semantics are unchanged, and the fast profile pays no append/allocation cost.
- The scenario holds allocation and retirement results full, fills the ROB,
  preserves a fifth request, completes out of order, drains in order, wraps
  indices, rejects a stale generation, recovers to a new epoch, rejects the old
  epoch completion, and resumes allocation/retirement. The second reusable
  instance allocates, completes, and retires independently.
- Exact result visibility follows from the per-tick Queue comparison. Host pops
  are proposed against both runtimes at the same matched boundary and commit
  through the normal external-Xfer path.

**Verification**
- The full scenario completes with byte/value-identical committed projections
  and identical ordered commit events after every compared tick.
- The generated source still contains one ROB specialization class and two
  independently owned state placements.
- The scan reference performs 1769 Work calls. Incremental activation performs
  182 Work calls, traverses 215 activation edges, and traverses 511 same-epoch
  closure edges for the same scenario.
- The complete Queue integration suite passes with the equivalence gate enabled.

**Source**
- User objective (2026-09-06): continuously validate that compiler-inferred
  activation is both correct and efficient while completing a real ROB/ISQ.

## Decision 0197: normal-list find lowers to a reusable oldest-ready ISQ

**Status:** Accepted and implemented for one four-entry, single-issue queue

**Context / Goal**
The frontend must not require a Table, Queue, register, marker, pop/full check,
or hardware-shaped selection object merely to select a free or oldest-ready
entry. At the same time, an ISQ must preserve readiness that arrives before
dispatch, react to resident wakeups, and keep state unchanged under output
backpressure.

**Decision (strong constraint)**
- `ac.find(values, where=..., key=...)` is an algorithmic intrinsic over an
  ordinary persistent Python list. Its result has `valid`, `index`, and `value`.
  Without a key it selects the first matching index; with a key it selects the
  minimum unsigned fixed-width key with stable index tie-breaking.
- The frontend emits `ac.var.match`, `ac.var.choose`, and a demand-driven
  `ac.var.read_element`. It does not emit Table operations. The selected value
  read is materialized only when `.value` is used, preventing dead reads from
  invalidating frozen footprint evidence.
- MLIR verifies the shaped lexical owner, 1..64 match domain, exact mask/index
  widths, direct same-owner provenance, count one, predicate result, policy,
  and min-key type. Storage selection rewrites the query and region terminators
  to existing `ac.table.match/choose` semantics.
- Rule ownership is inferred from writes, indexed reads, the searched list,
  and persistent-list captures inside the predicate. Boolean list zero images
  remain boolean rather than being rewritten to an integer marker.
- State reads and writes are independent compiler facts. A read-only owner is
  included in typed footprints and activation sources but excluded from
  transaction resources. Rule/firing verification recursively checks nested
  match/choose/get footprints.
- Generated stateful policies capture read-only Tables through const pointers.
  Transition prepare/publish/commit tuples continue to contain only writable
  owners. Structured and flat QueueGraph emitters share this distinction.
- Repeated index and valid projections of one choose operation are evaluated by
  one generated scan. Code generation caches the selection result instead of
  scanning the same candidates twice.
- `reusable_oldest_ready_isq.py` contains a four-entry `entries` list and a
  64-entry boolean readiness list. Three serial rules update readiness, select
  the first free slot for dispatch, and select the minimum-age ready entry for
  issue. The root places two instances of one specialization.
- Readiness events carry a tag and a boolean value. A false update closes tag
  reuse before a later true completion. Query-time readiness avoids a second
  resident readiness image and naturally reconciles wakeup-before-dispatch and
  same-epoch wakeup/dispatch at the next committed activation.
- Output Queue capacity, input retention, selected-entry clearing, Queue pops
  and pushes, reservations, and atomic commit remain compiler/runtime behavior;
  none appears in Python.

**Verification**
- Frontend tests prove raw output contains only generic `ac.var` collection
  queries, accepts read-only persistent-list captures and boolean list zero
  images, and rejects `find` on scalar state.
- ACIR lit proves verifier rejection and exact var-to-Table storage selection.
- Frozen ISQ evidence classifies `entries` and `ready_tags` as activation
  sources while only `entries` is a transaction state resource for issue.
- Generated C++ contains one ISQ specialization class, two placements,
  independent Table objects, one cached oldest-ready scan, and const read-only
  readiness capture.
- Runtime integration covers prior readiness, same-epoch readiness plus
  dispatch, resident wakeup, four-entry full retention, oldest-ready order,
  output backpressure without early clear, instance isolation, and false/true
  tag reuse.

**Source**
- User objective (2026-09-06): keep Python simple and serial while MLIR derives
  checks, backpressure, activation, atomic transactions, and reusable efficient
  simulation for a real ROB/ISQ.

## Decision 0198: reusable ISQ scan and activation are equivalent per tick

**Status:** Accepted and implemented for the complete Decision 0197 scenario

**Context / Goal**
An oldest-ready result sequence alone cannot prove the correctness of
incremental activation. A missing readiness wake or premature entry clear may
still produce the same eventual values while changing visibility time,
stranding an input, or corrupting a reused tag. The ISQ therefore receives the
same committed-projection comparison used by the circular ROB.

**Decision (strong constraint)**
- The gate constructs two independent copies of the generated dual-instance
  ISQ. The scan reference schedules all sixteen model dispatch rows each tick;
  the incremental model installs only the generated activation and Work-closure
  CSR plans.
- A test-only Process clock advances both systems without proposing or
  committing model state. Both runtimes use the validated profile.
- After every Xfer boundary, the gate compares all request, readiness, and
  result Queue contents; all eight resident entries; all 128 readiness bits;
  the current epoch; and the complete `{epoch,ObjectId,semanticChanged}` commit
  timeline.
- Host offers and result pops are applied to both matched boundaries. The
  scenario is unchanged from Decision 0197: prior and same-epoch readiness,
  resident wakeup, four-entry full state, retained fifth input, oldest-ready
  order, output backpressure, instance isolation, and false/true tag reuse.
- Performance counters are an exact regression contract for this scenario.
  Full scan performs 952 Work calls. Incremental activation performs 129 Work
  calls, 100 activation traversals, and 146 Work-closure traversals.

**Verification**
- Every compared Queue, entry, readiness bit, epoch, and commit event is equal
  after each step through the complete scenario.
- Output visibility and selected-entry invalidation therefore occur at the same
  commit boundary in scan and incremental execution.
- The complete Queue integration suite passes with the exact counter assertion.

**Source**
- User objective (2026-09-06): continuously inspect and prove correctness and
  efficiency while completing the compiler-driven ROB/ISQ flow.

## Decision 0199: typed rule summaries bridge the current single-condition subset

**Status:** Accepted and implemented; legacy string summaries retired by Decision 0219

**Context / Goal**
The existing rule pipeline carried guard, schedule, handshake, and effect
proofs primarily as strings. Replacing them directly with an `always` versus
`predicate` enum would improve type safety but would not identify then/else
paths, selected outputs, disjoint writes, or arbitration contenders. The first
step must therefore improve the current subset without misrepresenting summary
categories as final CFG semantics.

**Decision (strong constraint)**
- ACIR defines closed enums for rule guard kind, schedule kind, inferred check
  kind, effect kind, output-presence kind, state-access kind, index kind, and
  arbitration policy.
- Existing passes derive `ac.checks_typed`, `ac.effects_typed`,
  `ac.output_presence`, `ac.state_accesses`, `ac.guard_kind`,
  `ac.schedule_kind`, and `ac.arbitration_membership` for every lowered rule.
- Queue input availability is unconditional for candidate evaluation. Queue
  consumption, output production/capacity, and state writes carry the current
  total guard kind. State reads carry `always`. Read-only owners remain absent
  from transaction resources and arbitration membership.
- `state_accesses` describes read/replace/field-write access; it is deliberately
  not named a conflict class. `arbitration_membership` records one rule's owner,
  lexical rank, and policy; it is deliberately not an explicit contender graph.
- Firing and proof-carrying Transform IR preserve the typed summaries. The
  verifier reconstructs them independently from endpoints, condition, typed
  footprints, proposals, and lexical priority and rejects any mismatch.
- Legacy guard/check/handshake/schedule/effect strings were derived readable
  summaries during the phase-0 bridge. Decision 0219 removes them after all
  fixtures, verifiers, and QueueGraph consumers move to the typed contract.
- This decision does not complete typed paths. A `predicate` guard or presence
  enum does not identify a particular SSA condition or prove path disjointness.
  The next slice must add SSA `!ac.var<i1>` presence to output and state effects.
- Python is unchanged. No check, ready/full, presence, conflict, arbitration,
  reservation, or commit syntax is exposed.

**Verification**
- The phase-0 gate originally printed the typed summary beside its derived
  readable summary. Decision 0219 closure now proves the typed form is
  sufficient and rejects the retired string attributes.
- Replacing the derived predicate guard kind with always is rejected directly
  by the Firing verifier.
- ACIR lit passes 160/160 tests.
- Complete ROB and ISQ scan/incremental lockstep tests retain byte/value-equal
  state, identical commit timelines, and their exact performance counters.

**Source**
- User objective (2026-09-06): move readiness, backpressure, path, conflict,
  and atomic mechanics into MLIR while continuously checking that the frontend
  stays simple and the generated simulator stays efficient.

## Decision 0200: SSA presence closes the current single-condition rule path

**Status:** Accepted and implemented for the single-condition, 0/1-output subset

**Context / Goal**
Typed presence categories improve summary validation but do not identify the
actual condition that selects an output or state effect. The current rule slice
needs real SSA evidence before QueueGraph extraction, without adding presence,
ready/full, pop/push, or sink syntax to Python.

**Decision (strong constraint)**
- `ac.rule.output %value when %present ordinal N` and
  `ac.firing.output %value when %present ordinal N` are compiler-owned proof
  operations. They bind each returned value and exact output ordinal to an SSA
  `!ac.var<i1>` presence value.
- A firing-local `ac.table.propose` may carry `when %present`. Rule schedule
  resolution supplies the current rule condition to every proposal and output;
  an unconditional rule receives one constant-true condition at block entry so
  it dominates all authored proposals.
- Rule and Firing verifiers independently require exactly one condition whenever
  SSA path evidence exists, exactly one proof per output ordinal, returned-value
  identity, and proposal/output presence equal to that condition. Existing or
  forged disagreement is rejected before topology freeze.
- Rule-to-Firing lowering preserves the proof operations. Pure Firing
  canonicalization removes them only after proving the condition/output
  relationship needed by `ac.transform`.
- QueueGraph records `present` on every state-write plan and preserves ordered
  `{ordinal,value,present}` output-presence records. Its verifier requires the
  current subset's presence values to equal the block guard.
- Generated gfsim continues to use the equivalent total guard for this subset.
  This is permitted only because MLIR and QueueGraph verification prove that
  every selected output and state proposal has exactly that presence.
- Python is unchanged and retains the single `ac.var` value concept. No
  alternate variable spelling, presence marker, Queue check, transaction
  operation, or sink is added to the authoring surface.
- This decision does not admit independent proposal/output presence, a false
  path that consumes input without producing selected effects, multiple selected
  outputs, general CFG joins, pairwise conflict classes, or explicit contender
  arbitration.

**Verification**
- Guarded state-driven lowering prints one condition used by its Table proposal
  and output proof, then preserves both identities in QueueGraph JSON.
- Firing verification rejects missing, duplicate, mismatched, and invalid
  ordinal output evidence; schedule resolution rejects conflicting preexisting
  proposal presence.
- Unconditional multi-state lowering proves the synthesized constant-true
  condition dominates proposals authored earlier in the block.
- ACIR lit passes 160/160 tests; all native C++ suites pass 15/15; Python
  frontend/public API passes 90/90; Queue integration passes 23 tests with one
  optional skip.
- ROB and ISQ lockstep tests retain exact counters: ROB 1769/182 Work with
  215 activation and 511 closure traversals; ISQ 952/129 Work with 100
  activation and 146 closure traversals.

**Source**
- User objective (2026-09-06): infer atomic Queue/state path mechanics in MLIR,
  keep Python serial and simple, and preserve reusable high-performance gfsim.

## Decision 0201: conditional effects separate token acceptance from state commit

**Status:** Accepted and implemented for one-input, outputless early-return rules

**Context / Goal**
A blocking rule predicate and a stale-token discard are not the same operation.
ROB allocation and ISQ dispatch must retain their input when no capacity or free
entry exists, while a stale completion must consume its token without changing
state. Inferring the distinction from predicate spelling would be unsound, but
adding Queue or transaction markers to Python would violate the frontend
contract.

**Decision (strong constraint)**
- A trailing Python `if` without `else` retains its existing blocking meaning:
  false forms no candidate and input remains queued. An outputless rule with
  exactly one payload may instead use one ordinary early `return`; reaching it
  means that token has completed with no subsequent state effect.
- The frontend emits a constant-true `ac.rule.condition` for that input
  candidate and attaches the inverted early-return predicate as compiler-owned
  `when` on every following `ac.var.assign` or `ac.var.assign_element`. No new
  Python primitive, marker, ready/full check, pop/push, sink, or transaction API
  is introduced.
- Generic `ac.var` assignments accept optional `!ac.var<i1>` presence.
  Storage selection preserves it exactly on `ac.table.propose`; it never
  bypasses the storage-neutral variable family by emitting a Python-facing
  Table operation.
- `ACDataFlowAnalyzer` retains proposal presence in each state access
  footprint. Typed state effects/access summaries classify reads as always and
  writes from their own presence rather than copying the total candidate kind.
- Rule and Firing verification accepts a differing effect presence only when
  the candidate is constant true, the rule has exactly one input, and every
  differing state effect shares one SSA predicate. Presence must otherwise
  equal or imply the candidate. The current output proofs remain coupled; this
  decision does not admit optional outputs.
- QueueGraph treats `guard` as candidate presence and `state_writes[].present`
  as effect presence. Its verifier independently enforces the same narrow
  implication, arity, type, and shared-predicate constraints.
- Generated gfsim uses `nullopt` only for a stalled candidate. An engaged plan
  with absent writes consumes the selected input but produces no Table commit
  or semantic-change wake.
- State reservation and state commit are independent runtime facts. Generated
  plans retain the proposed indices as snapshot reservations even when a write
  is absent. Overlapping lexical writers therefore force re-evaluation against
  the later committed snapshot; disjoint indices do not block. An unselected
  reservation is cancelled after the selected Queue resources publish and is
  never converted into a Table proposal or commit event.
- The reusable ROB completion rule now uses early return for generation/epoch
  mismatch. A stale completion performs an input-only commit; a fresh completion
  atomically updates its entry and epoch owner. Allocation-full and ISQ-no-free
  paths keep the older blocking semantics.
- Exact predicate read-set inference remains follow-up work. The current
  reservation derives from each potential write index; read-only predicate
  owners still require analyzer-derived reservations before conservative
  self-assignments can be removed from every frontend example.

**Verification**
- Python frontend tests prove raw `ac.var.assign_element ... when` emission and
  absence of output obligations or sinks.
- ACIR lit covers presence preservation, typed footprint classification,
  candidate/effect separation, and rejection of non-i1, non-implying,
  zero-input, or multiple-predicate forms. The complete suite passes 160/160.
- gfsim unit tests prove absent single- and multi-owner writes create no Table
  commit, disjoint reservations proceed, and overlapping reservations stall
  until the conflicting writer is removed.
- The consume-only completion example consumes a stale token with no pending
  Table commit, then accepts a fresh token and updates state.
- Reusable ROB integration covers same-epoch allocation/completion
  revalidation, stale input-only commit, and absence of epoch/entry Table commit
  events on the stale path.
- ROB and ISQ scan/incremental lockstep retain their exact counters: ROB
  1769/182 Work, 215 activation, 511 closure; ISQ 952/129 Work, 100 activation,
  146 closure.

**Source**
- User objective (2026-09-06): keep Python parameterized and serial while MLIR
  infers atomic Queue checks, backpressure, path effects, and efficient reusable
  ROB/ISQ simulation.

## Decision 0202: analyzer-derived state snapshots replace conservative self-writes

**Status:** Accepted and implemented for top-level indexed reads in conditional effects

**Context / Goal**
Decision 0201 kept ROB completion serializable by writing `epoch = epoch`, which
forced the scalar owner into the write closure. That is correct but violates the
frontend goal: a read dependency must not require a fake assignment, and a
snapshot reservation must not become a Table commit or conflict with another
reader as though both were writers.

**Decision (strong constraint)**
- ACIR adds compiler-owned `ac.state.snapshot`. It records a Table, an exact
  static/dynamic index, the SSA predicate whose decision depends on that
  committed value, and enum-typed `RuleIndexKind`. The operation is legal only
  directly inside Rule/Firing, is never a write, and is not removable as a pure
  no-result operation.
- `ACDataFlowAnalyzer` walks backward from each conditional-effect presence that
  differs from the candidate. It follows the immutable Var def-use slice and
  derives every contributing top-level `ac.table.get`. Deduplication is by
  predicate/resource/index-kind/index identity.
- This slice preserves exact constant or statically safe full-domain dynamic
  indices and reserves the complete selected entry. A nested match/choose region
  read would require an out-of-region SSA index or an all-table approximation;
  it is rejected rather than silently weakened or overclaimed.
- Schedule resolution materializes the canonical snapshot ops after candidate
  and effect presence are fixed. `ac-verify-rule-closure` runs a fresh
  `ACDataFlowAnalyzer`, independently recomputes the expected set, and rejects
  missing, extra, reordered, or forged resource/index/predicate evidence before
  topology freeze.
- QueueGraph carries ordered `state_reservations` separately from
  `state_writes`. Every snapshot owner must be an activation source. A
  reservation-only owner must not appear in transaction resources or the Table
  Commit closure.
- Structured and flat gfsim codegen form the deterministic union of write and
  reservation owners in frozen Table declaration order. Writes remain optional;
  reservation-only owners receive no value proposal. Repeated module instances
  continue to share one implementation class and own independent Table objects.
- `SimTable::prepareTransaction` receives snapshot indices and optional write
  indices together. Snapshot/snapshot is compatible; overlapping snapshot/write
  conflicts; disjoint indices remain compatible; write/write retains the
  existing mode/field conflict rules. A reservation-only group is cancelled
  after Queue publication and never enters pending Table state, the validated
  commit timeline, or semantic-change activation.
- Python lexical call binding infers the whole persistent-state prefix through
  the last known state parameter. A referenced scalar state parameter therefore
  lowers to `ac.var.read` even when it has no assignment. Invalid interleaving of
  persistent and payload arguments still fails closed.
- The reusable ROB completion rule removes `epoch = epoch`. Its frozen plan has
  one real `entries` write, exact `entries[completion.index]` and `epoch[0]`
  snapshots, `epoch` as an activation source, and no epoch transaction resource.
- Candidate/output predicate snapshots and exact match/choose index sets remain
  follow-up work. An all-table ISQ bridge is not accepted as final efficiency
  evidence until same-tag correctness and continuous unrelated-update
  starvation/performance gates pass.

**Verification**
- Analyzer unit tests derive exact epoch and entry snapshot records from a
  conditional-effect predicate.
- ACIR lit checks typed snapshot materialization, QueueGraph JSON preservation,
  and closure rejection of missing or extra proofs; the full suite passes
  160/160.
- gfsim unit tests prove read/read compatibility, read/write conflict, disjoint
  progress, absent-write cancellation, and no phantom Table commit.
- Reusable ROB integration proves one shared class/two independent placements,
  same-epoch allocation/completion revalidation, and no state commit for a stale
  completion. Scan and incremental executions remain equal after every tick.
- The optimized ROB counters are 1769 scan Work calls, 182 incremental Work
  calls, 215 activation traversals, and 511 closure traversals. Removing the
  fake epoch write eliminates 34 closure traversals without changing results or
  the commit projection. ISQ remains 952/129 Work, 100 activation, 146 closure.

**Source**
- User objective (2026-09-06): derive state dependencies and atomic checks in
  MLIR, keep the Python ROB/ISQ serial and free of hardware mechanics, and
  continuously validate simulator efficiency.

## Decision 0203: match-evaluation snapshot sets close exact ISQ readiness dependencies

**Status:** Accepted and implemented for one `table.match` source and up to 64 dependency entries

**Context / Goal**
Decision 0202 derives exact top-level snapshot indices from conditional state
effects, but the oldest-ready ISQ decides whether it can issue by scanning one
entry list while reading source-tag readiness from another persistent list.
Reserving the entire readiness owner is correct but needlessly serializes
unrelated wakeups; rescanning after candidate construction is both slower and
can describe a different committed evaluation than the one that selected the
candidate.

**Decision (strong constraint)**
- `ac.var` remains the only ACIR variable-value family; no second spelling or
  Python variable constructor is introduced. The public compiler analysis is
  `ACDataFlowAnalyzer`; MLIR's generic dataflow solver remains an implementation
  detail of its private `Impl`.
- `ACDataFlowAnalyzer::stateSnapshots()` treats rule/firing candidate,
  output-presence, and state-proposal presence as roots. It records the source
  Table of a contributing `ac.table.match` as an all-entry snapshot because
  every entry participates in that scan.
- A region-local `ac.table.get` of a foreign Table inside that match predicate
  becomes compiler-owned
  `ac.state.snapshot_set @target from %match_mask for %presence`. The source is
  the exact mask produced by the same match evaluation; the operation is not a
  user marker, write, transaction resource, or independently committable
  effect.
- The local verifier requires the snapshot-set target to resolve within the
  same lexical ancestry, contain at most 64 entries, and be read inside the
  owning rule/firing's source match. Closure verification reruns
  `ACDataFlowAnalyzer` and compares resource, source mask, predicate, order, and
  set kind exactly before topology freeze.
- QueueGraph carries the record as `index_kind = "set"` with explicit source
  SSA identity. Its verifier requires that source to be the corresponding
  `table_match` expression and confirms the nested expression reads the target
  Table.
- Structured and flat gfsim codegen initialize one `uint64_t` dependency mask
  and OR each actually evaluated foreign read index into it inside the original
  match loop. No second `snapshot_entry` scan, dynamic allocation, module
  expansion, or duplicate implementation class is introduced.
- Runtime prepare receives scalar/all/set reservations through the same
  `uint64_t` snapshot mask. Snapshot/snapshot remains compatible; only an
  overlapping write conflicts. Reservation-only owners never enter the Table
  commit projection.
- The current operation deliberately names only match-evaluation sets. Choose
  key-region dependencies, field-level snapshots, general CFG joins, and
  representations beyond 64 entries remain follow-up work and must preserve
  exact evaluation provenance.

**Verification**
- Focused ACIR lit checks analyzer materialization, frozen QueueGraph JSON,
  in-scan mask codegen, absence of `snapshot_entry`, and C++20 compilation.
- The reusable ISQ plan reserves all scanned `entries` and exactly the
  dynamically read `ready_tags` set. Two source-tag reads produce two in-loop
  mask updates while repeated placements still share one generated class.
- Same-epoch readiness clear and issue on the same tag conflict and re-evaluate
  against committed state. A continuous stream of unrelated readiness writes
  does not starve issue; repeated source tags and tag 63 are covered.
- ACIR lit passes 161/161; native C++ suites pass 15/15, including CodeGen
  105/105; Python frontend/public API passes 91/91; Queue integration passes
  23 tests with one optional skip.
- The expanded ISQ lockstep scenario is equal after every tick and has exact
  counters: 1377 scan Work calls, 200 incremental Work calls, 162 activation
  traversals, and 238 Work-closure traversals. This supersedes the narrower
  Decision 0198 baseline of 952/129/100/146.

**Source**
- User direction (2026-09-06): keep Python parameterized, serial, and free of
  Queue checks or markers; infer exact atomic dependencies with MLIR dataflow
  and keep reusable gfsim modules efficient.

## Decision 0204: choose-key snapshot sets preserve exact evaluation provenance

**Status:** Accepted and implemented for transactional min/max selection

**Context / Goal**
Decision 0203 records foreign state read by a Table match predicate, but a
selection key may also consult persistent state. Associating that dependency
with the preceding match mask is incorrect: the key executes only for matched
candidates, and the match and choose are distinct committed evaluations.
Re-evaluating the key after candidate construction would likewise risk a
different snapshot and add another state scan.

**Decision (strong constraint)**
- `ACDataFlowAnalyzer` uses `TableChooseOp::getIndex()` as the canonical source
  for every foreign `TableGet` that contributes through the choose key region.
  The index and valid results describe one selection evaluation; choosing the
  index gives QueueGraph one deterministic source identity independent of which
  result the user expression consumes.
- `ac.state.snapshot_set` accepts either a `table.match` mask or the index
  result of a `table.choose` owned by the same rule/firing. Its local verifier
  rejects the choose valid result, cross-owner sources, and targets not read by
  the corresponding key region.
- A choose key may contain `TableGet` only under transactional Rule/Firing
  ownership. Shared non-transactional selection remains rejected because it
  has no inferred snapshot/prepare/publish/no-fail commit closure.
- QueueGraph accepts `table_choose_index` as snapshot-set provenance only when
  that expression's key region contains a read of the target Table. Resource,
  source, predicate, kind, and ordering remain independently reconstructed at
  closure verification.
- Generated gfsim initializes the dependency mask beside the cached choose
  evaluation. It tests the candidate bit first, records each foreign key-read
  index, and then evaluates the key. Unmatched entries contribute no
  reservation. The paired choose result reuses the same local selection cache,
  so no second choose loop or state scan is emitted.
- Python remains ordinary and serial. A key such as
  `key=lambda entry: priorities[entry.tag]` uses a normal persistent list;
  frontend IR remains `ac.var.choose` plus `ac.var.read_element` until storage
  selection. No snapshot, Queue, Table, readiness, or atomic marker is exposed.
- Field-sensitive reservations, general CFG joins, and snapshot sets beyond 64
  entries remain follow-up work.

**Verification**
- Native analyzer coverage proves one candidate yields ordered reservations for
  the scanned Table, a match-predicate foreign Table, and a choose-key foreign
  Table, with match-mask and choose-index sources kept distinct.
- ACIR verifier tests reject a snapshot set sourced from choose valid, reject a
  target absent from the key evaluation, and reject foreign Table reads in a
  shared non-transactional choose.
- End-to-end lit lowers min-key selection, preserves both set reservations in
  QueueGraph, generates exactly one mask update for each region-local read,
  emits no `snapshot_entry` scan, and compiles the result as C++20.
- Python frontend coverage lowers ordinary persistent-list key capture without
  introducing authored Table operations.
- ACIR lit passes 162/162; `ACDataFlowAnalyzerTest` passes 4/4; native C++
  suites pass 15/15 including CodeGen 105/105; Python frontend/public API passes
  92/92; Queue integration passes 23 tests with one optional skip.
- The reusable ROB and ISQ behavior and existing ISQ 1377/200/162/238 counters
  remain unchanged because their current selection key is entry-local.

**Source**
- User objective (2026-09-06): keep the authoring model simple and infer all
  atomic dependencies with MLIR dataflow while preserving efficient reusable
  gfsim modules.

## Decision 0205: field-qualified snapshots remove false same-entry conflicts

**Status:** Accepted and implemented for direct top-level Entry field reads

**Context / Goal**
Decisions 0202 through 0204 identify exact snapshot entry indices and dynamic
index sets, but every read still conflicts with every write to the same entry.
ROB/ISQ predicates commonly inspect only generation, epoch, ready, or valid
fields. Treating an unrelated field-merge as a conflict adds avoidable
re-evaluation and prevents later pairwise conflict analysis from being precise.

**Decision (strong constraint)**
- `ACDataFlowAnalyzer` propagates a direct `ac.var.get` field selection backward
  to the contributing `TableGet`. A complete Entry use expands to every field
  in declaration order; scalar entries use the single `$entry` field.
- `ac.state.snapshot` and `ac.state.snapshot_set` carry mandatory ordered
  `read_fields`. Their local verifiers reject empty, duplicate, unknown, or
  declaration-order-invalid fields. Closure verification independently
  recomputes and compares the field list with resource, index/source,
  predicate, kind, and order.
- QueueGraph preserves `fields` on each state reservation and validates them
  against the Table Entry payload. Generated plans therefore retain field proof
  after ACIR operations have disappeared.
- gfsim replaces a bare snapshot entry mask with `StateReservation`. Complete
  Entry reads use one entry mask. Partial reads use a second `uint64_t` whose
  bit position is `entry * declared_field_count + field`, plus the field count.
  Construction and conflict checking allocate no heap storage.
- A whole-entry read conflicts with any write to an overlapping entry. A
  replace write conflicts with every overlapping field-qualified read. A
  field-merge write conflicts only when the exact `(entry, field)` bit exists.
  Snapshot/snapshot remains compatible and reservation-only state still never
  enters the Table commit projection.
- Generated structured and flat policies use an entry-mask reservation for
  complete Entry reads and
  `StateReservation::forFields(entryMask, fieldMask, fieldCount)` for partial
  reads. Relation union preserves heterogeneous clauses exactly. Existing
  match/choose masks remain single-evaluation values; no state rescan is added.
- Partial relations require `entries * declared_field_count <= 64` and fail
  closed in ACIR, QueueGraph, and codegen when that bound is exceeded. Complete
  Entry and scalar reservations retain the existing 64-entry representation.
  A wider exact relation remains follow-up work.
- Python remains unchanged: field access is ordinary `entry.field`, persistent
  state remains ordinary lexical variables/lists, and no `read_fields`, Table,
  reservation, conflict, or atomic marker appears in the authoring surface.

**Verification**
- Conditional-effect lit derives only `read_fields ["value"]` from a struct
  Entry predicate, preserves it in QueueGraph JSON, emits field mask `2`, and
  compiles the generated C++20 model.
- Match/choose lit preserves complete source Entry fields and scalar `$entry`
  fields for dynamic snapshot sets across ACIR, QueueGraph, and gfsim codegen.
- gfsim tests prove a ready-field snapshot can coexist with a valid-only
  field-merge on the same entry, while overlapping ready writes and whole-entry
  replacements conflict. A heterogeneous relation reading `(entry0, ready)`
  and `(entry1, valid)` permits the two cross writes and rejects the two exact
  overlaps, proving there is no entry/field cross-product.
- Verifier lit rejects a 64-entry, two-field partial snapshot because its exact
  relation does not fit the current 64-bit representation.
- Reusable ROB integration proves completion reserves only
  `entries.{generation, epoch}` plus scalar epoch, retains one shared class/two
  independent instances, and keeps exact per-tick results and commit timeline.
- ACIR lit passes 162/162; `ACDataFlowAnalyzerTest` passes 4/4; native C++
  suites pass 15/15 including CodeGen 105/105; Python frontend/public API passes
  92/92; Queue integration passes 23 tests with one optional skip.
- ROB remains 1769/182 Work with 215 activation and 511 closure traversals. ISQ
  remains 1377/200 Work with 162 activation and 238 closure traversals.

**Source**
- User objective (2026-09-06): infer exact atomic checks in MLIR, keep Python
  simple and serial, and continuously improve high-performance reusable ROB/ISQ
  simulation.

## Decision 0206: serial early-return chains lower to one SSA effect presence

**Status:** Accepted and implemented for contiguous pre-effect guards

**Context / Goal**
Decision 0201 distinguishes a blocking rule predicate from an input token that
is accepted and discarded by one early return. Real completion logic usually
checks generation, recovery epoch, opcode class, and duplicate state in several
readable serial steps. Requiring authors to collapse those checks into one
large Boolean expression makes Python less clear without adding semantic
information for the compiler.

**Decision (strong constraint)**
- An outputless rule with exactly one Queue payload may contain one or more
  top-level `if condition: return` statements. They must form one contiguous
  chain and precede every persistent-state assignment. Pure local observations
  may precede the chain so the conditions can name committed snapshots.
- The frontend translates serial control flow only: it forms
  `not condition0 and not condition1 ...` as ordinary `!ac.var<i1>` SSA. It
  emits a constant-true rule candidate for unconditional input acceptance and
  attaches the conjunction as the shared presence of all later `ac.var`
  assignments.
- Conditions remain pure and total under existing expression/index verifiers.
  Non-contiguous returns and a state effect before the guard chain fail closed;
  the compiler never speculates a state mutation across a return.
- Storage selection preserves the compound presence on `ac.table.propose`.
  `ACDataFlowAnalyzer` follows every operand of the conjunction, derives all
  contributing snapshots and exact fields, and materializes the same verified
  snapshot/prepare/publish/no-fail commit closure used by a single return.
- QueueGraph and gfsim continue to see one candidate presence and one effect
  presence. A failed guard chain consumes the input with no state commit; a
  passed chain commits all selected state atomically. No runtime branch or
  repeated Work object is introduced.
- The rule may not yet combine this chain with a blocking trailing guard,
  multiple Queue payloads, selected output, `else`, or branch-local effects.
  Those shapes require explicit path joins rather than another syntactic
  flattening rule.
- Python gains no primitive, marker, Queue operation, or hardware type. Authors
  write ordinary `if` and `return`; `ac.var` remains the sole compiler variable
  family and `ACDataFlowAnalyzer` remains the public analysis surface.

**Verification**
- Frontend tests lower a two-return chain to one `ac.var.mul` conjunction and
  one conditional state assignment. Negative tests reject a local statement
  between returns and a persistent assignment before the chain.
- The reusable ROB completion rule uses separate generation and epoch early
  returns. Its Python still contains no source/sink, Queue, Table, ready/full,
  pop/push, reservation, or commit syntax.
- Reusable ROB integration retains two instances backed by one generated class,
  exact field snapshots, identical per-tick Queue/state images, and the same
  validated commit timeline.
- ACIR lit remains 162/162; native C++ suites remain 15/15 including CodeGen
  105/105; Python frontend/public API passes 94/94; Queue integration passes 23
  tests with one optional skip.
- ROB remains 1769/182 Work with 215 activation and 511 closure traversals. ISQ
  remains 1377/200 Work with 162 activation and 238 closure traversals.

**Source**
- User objective (2026-09-06): keep Python parameterized and serial while MLIR
  derives atomic checks, backpressure, path effects, and efficient reusable
  ROB/ISQ simulation.

## Decision 0207: complementary branch presence selects distinct state owners atomically

**Status:** Accepted and implemented for one outputless one-input `if/else`

**Context / Goal**
Early-return chains express input-only discard paths, but ordinary serial code
also chooses between useful state effects. Splitting one Python `if/else` into
two independently scheduled rules would duplicate input ownership, expose
arbitration artifacts, and permit only one half of the intended transaction to
observe backpressure. The branch must remain one functional candidate with
compiler-proven path-local effects.

**Decision (strong constraint)**
- One outputless rule with exactly one Queue payload may contain one top-level
  `if/else`. Each branch contains one or more assignments to persistent owners;
  every owner may be assigned at most once across both branches in this slice.
- The frontend emits the branch condition once as `!ac.var<i1>`, derives its
  Boolean complement with a typed compare against false, and attaches those two
  values as per-assignment presence. It emits one constant-true candidate so
  the input is accepted regardless of the selected branch.
- Rule, Firing, schedule-resolution, and QueueGraph verification independently
  admit at most two distinct conditional-effect presence values and require
  them to be structurally complementary. Two unrelated predicates, more than
  one pair, a non-i1 value, or a non-constant-true candidate fail closed.
- Storage selection preserves each branch presence on its own Table proposal.
  QueueGraph keeps both optional writes in one block. Generated gfsim computes
  one immutable plan and prepares only the owner whose proposal is present;
  input publication and that selected owner still share one
  prepare/publish/no-fail commit group.
- Different branches may not write the same owner yet because the current
  QueueGraph plan has one optional write slot per owner. A branch value or index
  may not depend on another branch-written owner; this prevents speculative
  serial assignment from masquerading as a value phi. Same-owner and dependent
  branches require explicit value/state join IR.
- This slice does not admit branch-local selected outputs, nested branches,
  multiple Queue inputs, or a combination with blocking/early-return guards.
- Python gains no marker or hardware primitive. The source uses ordinary
  `if/else` and lexical variables; `ac.var` and `ACDataFlowAnalyzer` remain the
  single compiler value and analysis concepts.

**Verification**
- Frontend tests lower one branch test into two complementary write presences
  and reject the same owner in both branches.
- ACIR lit proves complementary proposals through Rule-to-Firing lowering,
  frozen QueueGraph JSON, gfsim generation, and C++20 compilation. Existing
  invalid lit continues to reject two unrelated effect predicates.
- `branch_local_state.py` contains one input and two scalar lexical owners.
  End-to-end execution sends a left-selecting command followed by a
  right-selecting command and observes exactly `left=7,right=0`, then
  `left=7,right=9`.
- The generated plan contains one Firing, two optional state writes with
  different presence identities, and one input transaction. The Python source
  contains no Queue/Table/source/sink/ready/full/pop/push/atomic syntax.
- ACIR lit passes 163/163; native C++ suites pass 15/15 including CodeGen
  105/105; Python frontend/public API passes 96/96; Queue integration passes 24
  tests with one optional skip.
- Existing reusable ROB/ISQ lockstep behavior and counters remain unchanged:
  ROB 1769/182/215/511 and ISQ 1377/200/162/238.

**Source**
- User objective (2026-09-06): infer path-qualified atomic effects in MLIR,
  keep Python simple and serial, and preserve efficient reusable ROB/ISQ
  simulation.

## Decision 0208: scalar same-owner branches join before one state proposal

**Status:** Accepted and implemented

**Context / Goal**
Decision 0207 supports branch-local writes to distinct owners but rejects the
ordinary scalar form where both arms assign the same lexical variable. Keeping
two proposals for one owner would violate QueueGraph's one-write-slot contract
and turn mutually exclusive values into an artificial arbitration problem. The
branch values should join before storage and transaction selection.

**Decision (strong constraint)**
- ACIR adds compiler-owned pure `ac.var.select %condition, %true, %false`. The
  condition must be `!ac.var<i1>` and both values plus the result must have one
  exact `ac.var` type. This is an IR value join, not a Python primitive or a
  Queue selector.
- When one Python `if/else` assigns the same scalar owner exactly once in each
  arm, the frontend evaluates both pure branch expressions from the same
  committed inputs, emits one `ac.var.select`, and emits one unconditional
  `ac.var.assign` of the joined value. The rule candidate remains constant true.
- Indexed same-owner assignments remain rejected because joining values alone
  does not join two potentially different indices. More than one assignment in
  an arm, non-complementary conditions, and cross-dependencies on another
  branch-written owner also fail closed.
- Storage selection lowers the single generic assignment to one Table proposal.
  Rule/Firing/QueueGraph therefore retain one owner, one write slot, one lexical
  arbitration membership, and one prepare/publish/no-fail commit.
- QueueGraph represents the join as typed `value_select` with condition,
  true-value, and false-value SSA operands. Its verifier checks arity, i1
  condition, and exact value/result types.
- gfsim C++ emits one ternary expression. ACIR-to-PYC QueueGraph lowering emits
  one `pyc.select`, preserving the same vendor-neutral combinational semantics.
- Python remains ordinary `if/else` assignment and gains no `select`, mux,
  state, Queue, or atomic constructor.

**Verification**
- ACIR parser/verifier tests accept scalar and struct `ac.var.select` and reject
  a non-i1 condition. QueueGraph lit verifies `value_select`, generated C++20
  ternary code, and PYC `pyc.select` lowering.
- Frontend coverage proves two same-owner arms generate one `ac.var.select`, one
  `ac.var.assign`, and no second owner proposal.
- `branch_join_state.py` selects direct value 9 on the true arm and incremented
  value 8 on the false arm. End-to-end generated gfsim execution observes both
  results while the frozen plan retains exactly one `total` state write whose
  presence equals the rule candidate.
- ACIR lit passes 165/165; native C++ suites pass 15/15 including CodeGen
  105/105; Python frontend/public API passes 96/96; Queue integration passes 25
  tests with one optional skip.
- Reusable ROB/ISQ behavior and counters remain unchanged: ROB
  1769/182/215/511 and ISQ 1377/200/162/238.

**Source**
- User objective (2026-09-06): keep Python serial and parameterized, infer
  branch/state transaction structure in MLIR, and preserve efficient reusable
  ROB/ISQ simulation.

## Decision 0209: indexed same-owner branches join index and value

**Status:** Accepted and implemented

**Context / Goal**
Decision 0208 joins scalar branch values before one state proposal. Persistent
lists require the same rule for both the value and the selected element: two
branch proposals would recreate an artificial same-owner conflict, while
joining only the value could write the correct value to the wrong index.

**Decision (strong constraint)**
- If both complementary arms assign one persistent list exactly once, the
  frontend emits one typed `ac.var.select` for the two values and a second
  `ac.var.select` for the two indices. It emits one unconditional
  `ac.var.assign_element` using both joined results.
- Both source indices must have one exact integer Var type. Each index is
  independently subject to the existing constant-range or full-`2^N` domain
  proof before the join. A scalar/list shape mismatch fails closed.
- Branch expressions are still pure and total in this flattened slice. Both
  index/value expressions may be computed eagerly from the same committed
  inputs, but only the joined index/value pair becomes a state proposal.
  Branch-dependent reads requiring lazy evaluation remain explicit-CFG work.
- Storage selection preserves one dynamic Table proposal. QueueGraph verifies
  both `value_select` expressions and carries one state write; gfsim evaluates
  two C++ ternaries and prepares exactly the selected entry.
- The unselected index is not reserved or written merely because it appeared in
  the alternate branch expression. Any committed state reads used to compute a
  branch expression remain independently derived snapshot dependencies.
- Python remains ordinary list indexing and `if/else`; no select, phi, Table,
  Queue, reservation, or atomic primitive is added.

**Verification**
- Frontend coverage proves indexed branch assignment emits exactly two
  `ac.var.select` operations and one `ac.var.assign_element` with no conditional
  proposal presence.
- ACIR/QueueGraph/gfsim lit preserves two typed `value_select` records, one
  dynamic state proposal, and C++20-compilable ternary code.
- `indexed_branch_join.py` sends a false-arm command selecting entry 1 and
  observes value 8, then sends a true-arm command selecting entry 3 and observes
  value 9. Entry 3 remains zero after the first command and entry 1 remains 8
  after the second.
- ACIR lit passes 165/165; native C++ suites pass 15/15 including CodeGen
  105/105; Python frontend/public API passes 97/97; Queue integration passes 26
  tests with one optional skip.
- Existing reusable ROB/ISQ lockstep behavior and counters remain unchanged:
  ROB 1769/182/215/511 and ISQ 1377/200/162/238.

**Source**
- User objective (2026-09-06): infer state and transaction structure in MLIR
  from simple serial Python while retaining high-performance reusable ROB/ISQ
  simulation.

## Decision 0210: optional output presence qualifies backpressure independently

**Status:** Accepted and implemented for one stateful input/output rule

**Context / Goal**
Prior rules either produced their sole output whenever a candidate fired or
produced no output at all. A filter-like stateful rule must sometimes consume
an input and commit bookkeeping without producing a token. Treating its output
as always present would incorrectly let output backpressure retain a token that
the functional rule has already discarded.

**Decision (strong constraint)**
- A one-input stateful rule may end with
  `if condition: return value` followed by `return`. Preceding state assignments
  are selected on both paths; the returned Queue value is selected only when
  the condition is true.
- The frontend emits a constant-true candidate, candidate-qualified generic
  state assignments, and compiler-owned `ac.rule.output %value when %condition`.
  No optional/ready/full/sink marker is added to Python.
- Rule and Firing verifiers allow output presence to differ from the candidate
  only for exactly one input and a proven constant-true candidate. Missing,
  false-candidate, invalid-type, or forged output presence fails closed.
- Effect and check inference derive output production and output capacity from
  the actual output SSA presence rather than the total candidate. Typed summary
  verification independently reconstructs the predicate-qualified output facts.
- Schedule resolution preserves a valid preexisting output proof instead of
  replacing it with candidate presence. Rule-to-Firing lowering keeps the exact
  value/ordinal/presence identity through QueueGraph.
- Generated gfsim represents the output as `std::optional`. Output capacity is
  prepared only when presence is true. The false path may consume input and
  commit state while the output Queue is full; the true path stalls input and
  state together until capacity becomes available.
- This slice retains one output and one input. Multiple selected outputs,
  multi-input discard paths, and optional pure transforms remain follow-up work.

**Verification**
- Frontend tests prove ordinary Python emits one independent `ac.rule.output`
  presence while state assignment remains candidate-qualified.
- ACIR lit verifies predicate output-capacity/effect summaries, Rule-to-Firing
  presence preservation, QueueGraph JSON, optional gfsim output generation, and
  C++20 compilation. Invalid Firing coverage rejects optional output under a
  false candidate.
- `optional_output_state.py` is executed with its output Queue deliberately
  held full. A false-presence input is consumed and increments count from 0 to
  1; a true-presence input remains queued with count unchanged, then consumes
  and increments to 2 only after capacity is released.
- ACIR lit passes 166/166; native C++ suites pass 15/15 including CodeGen
  105/105; Python frontend/public API passes 98/98; Queue integration passes 27
  tests with one optional skip.
- Reusable ROB/ISQ behavior and counters remain unchanged: ROB
  1769/182/215/511 and ISQ 1377/200/162/238.

**Source**
- User objective (2026-09-06): infer atomic output backpressure in MLIR from
  simple serial Python and preserve efficient reusable simulation.

## Decision 0211: static bitfield schemas share one immutable semantic core

**Status:** Accepted and implemented for 1–64-bit scalar schemas

**Context / Goal**
Issue #39 requires instruction and protocol decodes to retain static widths,
named overlapping bit views, immutable updates, and cross-backend parity. The
existing pyCircuit `BitfieldSpec` already provides the intended user meaning,
but copying its validation into Agentic Circuit would create two subtly
different layout contracts. Importing one public frontend from the other would
also collapse distribution boundaries.

**Decision (strong constraint)**
- `ac.bits[N]` and the `ac.uN` convenience names denote the same exact unsigned
  fixed-width value for `1 <= N <= 64`. Python slices remain half-open;
  concatenation is explicitly MSB-first; insertion is immutable
  read-modify-write. Width zero, dynamic width, and widths above 64 fail closed
  until every runtime/backend has a wide-bitvector representation.
- Logical Python `bool` and `ac.u1` continue to lower to `i1` in this additive
  slice. They are not implicitly separated under contract epoch `0.5`; doing
  so requires a later hard-break decision and coordinated epoch transition.
- Signedness remains an explicit interpretation. Existing `s8/s16/s32/s64`
  aliases carry storage width only and do not claim signed comparisons or
  implicit signed conversion.
- Bitfield declarations use closed `(msb, lsb)` intervals. Declaration overlap
  is legal because fields are alternate read views. One update rejects
  repeated or overlapping selected fields before emitting IR. Named reads and
  multi-field reads lower to `ac.var.extract` and MSB-first `ac.var.concat`;
  updates lower to one or more `ac.var.insert` operations.
- `_pycircuit_semantics.BitfieldLayout` is the single immutable validation and
  fingerprint implementation used by both Python distributions. It orders
  field names by UTF-8 bytes and hashes canonical kind/version/width/range
  metadata. The distributions keep separate public namespaces and wrappers.
- ACIR declares `ac.bitfield` inside `ac.type_scope`. Its verifier independently
  validates width, canonical field order, unique names, bounds, and SHA-256.
  Bitfield-derived extract/concat/insert operations retain schema, fingerprint,
  and selected-field provenance; their verifiers resolve the declaration and
  reject stale or forged field ranges before topology freeze.
- Existing flat structs remain nominal. The target recursive descriptor uses
  nominal identity for named struct/enum declarations and structural identity
  for tuples/fixed arrays, with stable layout fingerprints. That descriptor
  migration is a follow-up slice and does not change aggregate identity here.
- Contract epoch remains `0.5`: this is an additive declaration/provenance
  capability and lowers to bit operations already admitted by epoch `0.5`.
  Compilers without the capability reject the new op instead of silently
  accepting different semantics.

**Verification**
- Shared-core and existing pyCircuit bitfield tests prove one immutable layout,
  order-independent fingerprints, overlapping views, and overlapping-update
  rejection.
- Agentic frontend tests cover a 32-bit schema containing u3/u5/u17 fields,
  attribute and indexed reads, explicit MSB-first multi-field selection,
  immutable multi-field update, invalid static schemas, unknown fields, width
  mismatch, and overlapping writes.
- ACIR lit accepts canonical schema/provenance and rejects stale fingerprints,
  invalid bounds/order, mismatched extract ranges, and mismatched concat field
  widths.
- `bitfield_decode_pipeline.py` executes the resulting extract/concat/insert
  QueueGraph through generated gfsim C++. `bitfield_scalar_pipeline.py`
  produces the same value on the same cycle in generated PYC C++ and Verilator.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): keep the Python frontend simple and serial,
  derive semantics in verified MLIR, and preserve efficient shared backend
  implementations before DavinciOO bringup.

## Decision 0212: recursive value descriptors precede aggregate lowering

**Status:** Accepted and implemented for descriptor identity, flat payload discovery, and immutable tuple/value-array type admission

**Context / Goal**
The Agentic frontend historically passed MLIR type spelling strings through
payload discovery, rule parsing, QueueProgram construction, and expression
lowering. That makes nested aggregate validation depend on string parsing and
cannot preserve the semantic distinction between logical bool and one bit.
Issue #39 requires a recursive immutable type representation before recursive
aggregate lowering is admitted.

**Decision (strong constraint)**
- `_pycircuit_semantics` owns immutable `BoolType`, `BitsType`, `EnumType`,
  `ValueField`, `StructType`, `TupleType`, and `ArrayType` descriptors. Every
  descriptor provides canonical JSON-compatible identity, stable SHA-256,
  recursive bit width, and an explicit MLIR rendering boundary.
- `BoolType()` and `BitsType(1)` are distinct compiler identities even though
  Decision 0211 keeps both serialized as `i1` in epoch `0.5`. This preserves
  the information needed for a future explicit bool/u1 hard break without
  changing current backend semantics.
- Named enums and structs are nominal and include their declaration name in
  identity. Enum encoding follows declaration order with the minimum positive
  width. Tuple and fixed array identity is structural. Struct field order is
  semantic and contributes to packing and fingerprinting.
- Fixed `ArrayType(length, element)` is a value aggregate. It is not the
  persistent Python `list` selected to `ac.var`/Table storage by Decision 0172.
  It renders as `!ac.value_array<N x T>`; existing `!ac.array` remains a static
  topology collection whose elements are Queue/Var or another topology
  collection. Builtin MLIR tuple is an immutable payload only when every
  element is recursively immutable.
- The first migration replaces scalar annotation parsing and flat `@ac.struct`
  payload discovery with descriptors. Existing QueueProgram and expression
  records may render MLIR strings only through descriptor methods during this
  staged migration; they are not a second type authority.
- `ac-queue-cxxgen.py`, CMake build/install Python trees, isolated capture, CI,
  and packaging carry the shared semantic package explicitly. A repository
  tool may not depend on an ambient checkout or installed copy.
- The broad PYC test flow always runs the real `ac-freeze-topology` pass before
  QueueGraph-to-PYC. Tests compile all generated translation units rather than
  assuming a monolithic C++ file. Framework tests derive the DavinciOO-shaped
  workload from tracked framework goldens and do not read consumer trace trees.
- Contract epoch remains `0.5` because this slice changes internal type
  representation and test orchestration, not serialized value semantics.

**Verification**
- Recursive descriptor tests cover nested nominal structs/enums, structural
  tuple/array identity, recursive width, stable fingerprints, invalid empty or
  zero shapes, and distinct bool/u1 identity with current identical `i1`
  rendering.
- ACIR type tests distinguish `!ac.value_array` from topology `!ac.array`,
  accept immutable tuple/value-array Queue and Var payloads, and reject zero
  length or runtime-reference elements.
- Agentic frontend tests prove parsed payload fields retain descriptors before
  ACIR rendering while all existing flat payload output stays byte-compatible.
- Python frontend passes 184 tests with two optional skips; Queue/gfsim
  integration passes 29 tests with one optional skip; the broad PYC C++ and
  Verilator suite passes 13/13.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): continuously simplify the Python authoring
  surface, move inference into compiler analysis, and keep reusable simulation
  and RTL generation efficient enough for a complete DavinciOO Core.

## Decision 0213: nested nominal structs preserve recursive value semantics

**Status:** Accepted and implemented for acyclic nested structs

**Context / Goal**
Decision 0212 creates recursive descriptors and admits recursive immutable
types in ACIR, but the executable frontend still accepts only scalar struct
fields. DavinciOO packets require nested decode, dependency, ROB, and execution
metadata without flattening every field into the Python API.

**Decision (strong constraint)**
- One `@ac.struct` field may reference another nominal `@ac.struct` in the same
  source unit. Resolution is declaration-order independent and uses the
  immutable `StructType` graph. Recursive cycles fail before ACIR publication.
- Field access and immutable replacement remain ordinary chained Python
  attribute access and `.with_fields(...)`. The frontend lowers each level to
  existing typed `ac.var.get`/`ac.var.with`; it adds no nested-record marker or
  hardware-specific Python object.
- ACIR retains one nominal declaration for every struct and recursively
  computes DLTI size/alignment. Field order remains semantic. QueueGraph keeps
  top-level field names/types and validates that every nested payload reference
  resolves exactly once and that the payload dependency graph is acyclic.
- gfsim emits one C++ struct per nominal payload in dependency order, regardless
  of Python declaration order. Nested members use the shared struct type; they
  are not flattened or duplicated per Queue/module instance.
- QueueGraph-to-PYC recursively computes packed width and field offsets. Nested
  reads and replacements lower to the same vendor-neutral `pyc.extract` and
  `pyc.concat` semantics used by flat structs. Generated C++ and Verilog must
  agree on cycle, packed value, and backpressure.
- Contract epoch remains `0.5`: nested structs were already immutable nominal
  ACIR types; this slice completes frontend and backend support without
  changing existing flat payload semantics.

**Verification**
- Frontend tests resolve an outer struct declared before its nested dependency,
  retain the recursive descriptor, emit typed chained get/with operations, and
  reject a two-struct cycle before ACIR.
- QueueGraph verification rejects recursive payload plans; gfsim generation
  emits the nested C++ type before the outer type and executes a wrapping u3
  nested update while preserving sibling fields.
- PYC recursively packs the two-level payload to 26 bits. Generated PYC C++ and
  Verilator produce the same updated value on the same cycle.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): use Pythonic struct/class values while MLIR
  derives verified implementation and efficient reusable module behavior.

## Decision 0214: standard Python enums lower to nominal encoded values

**Status:** Accepted and implemented for nested enum fields and equality

**Context / Goal**
DavinciOO control packets need nominal states and opcode classes that cannot be
accidentally mixed merely because their bit widths match. Adding a hardware-
named Python enum constructor would duplicate the standard language concept
and make the frontend less Pythonic.

**Decision (strong constraint)**
- The Python surface uses standard `enum.Enum`. Agentic adds no `ac.enum`
  constructor or marker. The first slice requires explicit integer members
  contiguous from zero in declaration order; aliases, sparse values, methods,
  `auto()`, and custom encodings fail closed.
- `EnumType` retains nominal class identity, enumerant order, and the minimum
  positive encoding width. A struct field may carry that descriptor recursively.
- ACIR uses canonical `ac.enum @Name enumerants [...]` declarations and pure
  `ac.var.enum @types::@Name "MEMBER"` values. Verifiers resolve the nominal
  declaration, require an existing member, require the exact result type, and
  permit only equality/inequality comparisons between one enum type.
- QueueGraph stores each enum name, ordered member list, and encoding width.
  Enum constants store both member and ordinal; plan verification recomputes
  the width and rejects missing, duplicated, stale, or inconsistent metadata.
- gfsim emits one `enum class` using the narrowest standard unsigned storage
  class and reuses it in every nested payload/module instance. It does not
  lower enums to untyped integers in architecture-model C++.
- QueueGraph-to-PYC uses the explicit ordinal and exact width for packing and
  comparison. Generated PYC C++ and Verilog therefore share the same nominal-
  frontend encoding without adding a backend-only enum interpretation.
- Contract epoch remains `0.5`: the new declaration/value operations are an
  additive capability and existing integer/struct semantics do not change.
  Older compilers reject the unknown operation rather than accepting another
  encoding.

**Verification**
- Frontend tests retain a standard Python enum inside a nested descriptor,
  lower member values and equality, and reject sparse encoding or ordered
  comparison before ACIR.
- ACIR lit verifies canonical assembly/bytecode and rejects unknown members,
  mismatched nominal results, and ordered enum comparison.
- The enum QueueGraph contains the exact three-member/two-bit encoding.
  Generated gfsim executes WAIT-to-RUN replacement and equality; PYC C++ and
  Verilator produce the same 26-bit packet on the same cycle.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): keep frontend types Pythonic and nominal while
  compiler analysis inserts verified implementation semantics.

## Decision 0215: tuple and fixed value-array fields lower as packed immutable aggregates

**Status:** Accepted and implemented for struct fields, construction, and static indexing

**Context / Goal**
Decision 0212 defines structural tuple and fixed value-array descriptors, but
type admission alone does not let DavinciOO packets construct, rearrange, or
select aggregate fields. The Python surface must remain ordinary typed values;
Queue checks, mutable hardware containers, and per-element backend objects are
not acceptable authoring requirements or simulation costs.

**Decision (strong constraint)**
- A struct field may use `tuple[T0, ...]` or `ac.array[N, T]`, where every
  element is recursively immutable, `N` is a positive static integer, and the
  complete packed value is at most 64 bits in the current gfsim slice.
  Persistent Python `list[T]` remains lexical `ac.var` state and is never
  inferred as a fixed value array.
- Ordinary tuple or list literals construct tuple/value-array values. Literal
  arity must exactly match the descriptor. Subscription is permitted only with
  a compile-time integer in range. Python gains no Queue operation, aggregate
  marker, mutable hardware container, or explicit packing API.
- ACIR uses pure typed `ac.var.tuple`, `ac.var.array`, and `ac.var.element`.
  Their verifiers require exact operand element types, exact arity, a static
  in-range index, and a result matching the selected element. Unsupported
  dynamic shape or index fails before Frozen ACIR.
- QueueGraph records one canonical aggregate entry containing structural type
  identity, kind, ordered element identities, logical length, and recursive
  packed width. Payload fields carry their proven width. Verification rejects
  missing, duplicate, recursive, wider-than-64-bit, or width-inconsistent
  metadata before either backend consumes it.
- gfsim stores each admitted aggregate field as one `UInt<N>`. Construction is
  MSB-first bit concatenation and indexing is a constant bit extraction.
  Type-aware conversion recursively packs/unpacks standard enum and nominal
  struct elements around those operations; it does not allocate a runtime
  tuple/array or expand the field per module instance. QueueGraph-to-PYC emits
  the same vendor-neutral `pyc.concat` and `pyc.extract` operations, preserving
  identical layout in C++ and Verilog.
- Width derivation uses checked addition and multiplication in both the MLIR
  extractor and independent QueueGraph verifier. Arithmetic overflow, an
  aggregate wider than 64 bits, swapped tuple operand types, wrong arity, or a
  slice crossing an element boundary fails before code generation.
- Qualified enum/struct references used while computing recursive aggregate
  widths resolve through the enclosing `ac.type_scope`; declaration-order-
  independent nested structs and nominal enums therefore remain valid.
- Contract epoch remains `0.5`: these operations add a previously rejected
  immutable aggregate capability without changing existing scalar, enum, or
  nested-struct encodings.

**Verification**
- Frontend tests retain tuple/array descriptors, lower literal construction
  and seven static selections, and reject short literals, dynamic lengths, and
  out-of-range indices. The public example carries an exact 28-bit packet.
- ACIR positive/negative lit verifies the three aggregate operations and their
  malformed type/shape/index cases. QueueGraph C++ tests reject tuple width,
  value-array shape, aggregate-width, and payload-field-width forgeries.
- Generated gfsim C++ executes `(5, 17)` to `(6, 18)`, rotates lanes
  `(1, 2, 3, 4)` to `(2, 3, 4, 1)`, and selects the original lane 2. The PYC
  C++ and Verilator models produce the same 28-bit value on the same cycle.
- A 13-bit recursive payload covers `tuple[Mode, u3]`,
  `tuple[Header, u3]`, and `array[2, Mode]`; gfsim, PYC C++, and Verilator all
  transform the packed value `2962` to `7125`.
- Full gates pass: Agentic frontend 191 tests with two optional skips; CLI
  53/53; Queue/gfsim 32 tests with one optional skip; PYC C++/Verilator 17/17;
  ACIR lit 173/173; native CTest 20/20; pyCircuit unit 36/36; Agentic contracts
  37/37; decision status 215 verified rows with zero deferred.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): keep aggregate authoring Pythonic, infer and
  verify layout in MLIR, and preserve packed reusable backend implementations
  before complete DavinciOO Core bringup.

## Decision 0216: QueueProgram retains recursive descriptors until ACIR rendering

**Status:** Accepted and implemented

**Context / Goal**
Decision 0212 introduced immutable recursive descriptors, but the Queue
frontend still converted most annotations to MLIR strings before QueueProgram
construction. Queue, state, memory, Table, rule, and module analysis then used
`startswith`, slicing, and symbol-spelling reversal as a second informal type
system. This made scope spelling observable and would prevent the bounded
constraint domain from attaching facts to one stable value-type identity.

**Decision (strong constraint)**
- Every semantic type-bearing QueueProgram record stores a `ValueType`:
  Queue payloads, rule state reads/writes/finds/owners, persistent variables,
  Table entries, memory data, slots, fanout records, and reusable module
  state/input/output signatures. Persistent `list[T]` remains descriptor `T`
  plus a static entry count and is not converted to `ArrayType`.
- `_ExpressionEmitter.emit()` accepts an optional `ValueType` expectation and
  returns `(SSA name, ValueType)`. Root, state, Table, slot, candidate,
  selection, and find maps carry descriptors. Type identity uses descriptor
  equality; integer width and aggregate structure use descriptor APIs rather
  than parsing MLIR spelling.
- `_render_type(ValueType)` is the only Python Queue frontend call site for
  `ValueType.mlir()`. It is used solely while producing ACIR declarations,
  signatures, and operation text. An AST contract test rejects direct `.mlir()`
  calls elsewhere and rejects `str` annotations on the type-bearing records.
- The legacy direct gfsim C++ adapter accepts `ValueType` and selects storage
  through descriptor classes. C++ QueueGraph continues to use MLIR type strings
  only after the verified ACIR parser boundary; Python descriptors do not cross
  that language/process boundary.
- `BoolType()` and `BitsType(1)` remain distinct descriptor identities and
  fingerprints. Named epoch-0.5 compatibility helpers preserve every existing
  `i1` equality, integer-width, condition, field, state, and module boundary,
  so this internal migration neither performs nor prevents the separately
  decided bool/u1 hard break.
- Descriptor canonical identity and fingerprint are independent of the MLIR
  symbol scope selected by the printer. No QueueProgram JSON/pickle format is
  introduced.
- Contract epoch remains `0.5`: emitted ACIR and accepted behavior are
  unchanged; the earlier string representation was private Python compiler
  state.

**Verification**
- Frontend tests inspect Queue, Memory, Table, Slot, Var, and rule-owner records
  and require `ValueType` instances. Repeated parsing retains equal but distinct
  descriptor objects with stable fingerprints.
- Bool/u1 tests retain distinct internal descriptors while lowering logical
  `not` and bitwise `~` to their existing operations and common `i1` spelling.
- Static AST contracts require the single renderer call site and descriptor
  annotations across QueueProgram, ExpressionEmitter, nested module records,
  and the direct C++ adapter.
- Full gates pass: Agentic frontend 194 tests with two optional skips; CLI
  53/53; direct Python gfsim codegen 11/11; Queue/gfsim 32 tests with one
  optional skip; PYC C++/Verilator 17/17; ACIR lit 173/173; native CTest 20/20;
  pyCircuit unit 37/37; Agentic contracts 42/42; decision status 216 verified
  rows with zero deferred.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): make compiler analysis, rather than frontend
  spelling, own type and transaction inference before complete DavinciOO Core
  bringup.

## Decision 0217: bounded constraints are recomputed by ACDataFlowAnalyzer

**Status:** Accepted and implemented

**Context / Goal**
Widths, fixed shapes, slices, and topology loops need deterministic static
evaluation, while a non-power-of-two persistent collection must accept a
dynamic index whenever the compiler can prove it safe. Requiring every index
type to span exactly the collection size rejects safe programs; trusting a
frontend range marker would make raw or forged ACIR able to bypass the semantic
boundary.

**Decision (strong constraint)**
- The shared semantic core defines `Constant`, `FiniteSet`, `ClosedInterval`,
  and `Unknown`, plus a typed `ValueConstraint`. Constraints are analysis
  facts, not `ValueType` members, and do not affect descriptor identity,
  fingerprints, ACIR serialization, or specialization identity.
- Python static evaluation uses the domain for width, fixed value-array length,
  aggregate index, slice/insert bounds, and enum declaration coverage. These
  sites must resolve to constants before ACIR emission. Topology `range`
  expansion uses the same deterministic 10,000-iteration cap.
- Runtime bit-expression facts preserve exact `ac.var` width semantics:
  add/subtract/multiply wrap modulo (2^N), logical overshift yields zero, and
  finite/Cartesian propagation retains at most 64 values. Loss of precision
  widens conservatively; it never creates a narrower unsafe proof.
- `ac.var` remains the only variable family. There is no `ac.variable` alias,
  Python variable constructor, constraint marker, range annotation, or
  frontend ready/full/pop/push check.
- The public compiler analysis is `ACDataFlowAnalyzer`. MLIR's generic
  `DataFlowSolver` is contained only in its private implementation. The
  analyzer propagates value constraints across ACIR SSA and verifies dynamic
  `ac.var` and Table indices before rule lowering and topology freeze.
- Local operation verifiers retain structural/type and direct-constant checks.
  Whole-model `ac-verify-value-constraints` owns dynamic proof obligations.
  Freeze invokes the same verification, and QueueGraph independently
  recomputes constraints from its expression plan so mutated Frozen ACIR fails
  closed.
- A `u2` index is valid for five entries because `[0, 3]` is contained in
  `[0, 4]`. An unconstrained `u3` index is rejected for five entries, while a
  constant expression, mask, select, or priority/choose result may be accepted
  when its inferred domain proves the bound.
- This slice remains path-insensitive. Dynamic aggregate indexing or
  extract/insert, guard-derived refinement, runtime loop termination, general
  enum `if/elif` exhaustiveness, and masked matching remain independent
  follow-up work.

**Verification**
- Semantic-core tests cover canonical identity, typed bits/bool/enum facts,
  join/meet behavior, finite-set caps, modular wrap, overshift zero, and
  conservative widening. A regression locks interval absorption so the MLIR
  sparse lattice remains monotonic.
- Frontend tests cover computed widths, shapes, aggregate indices, slices,
  inserts, static loop caps, constant and typed expression facts, safe dynamic
  non-power-of-two indices, and constant out-of-range early rejection.
- `ACDataFlowAnalyzer` tests cover type domains, constants, bit operations,
  select, comparison, priority encode, and lattice laws. ACIR lit verifies safe
  and unsafe `ac.var`/Table indices before and after rule lowering and freeze.
- QueueGraph tests reject forged constraint plans and independently prove the
  admitted dynamic index cases. Generated gfsim executes a five-entry
  persistent Python list indexed by `u2` without frontend range checks.
- Full closure passes: Agentic frontend 200 tests with two optional skips; CLI
  53/53; ACIR lit 176/176; native CTest 20/20; fresh integrated pyc6/AC build;
  six PYC C++/Verilator integration tests plus four generated primitive cases;
  pyCircuit unit 43/43; Agentic contracts 42/42; API hygiene, strict docs, and
  217 decision-status rows with zero deferred.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): keep Python serial and marker-free, derive
  transaction and value safety in MLIR, use only `ac.var`, and expose
  `ACDataFlowAnalyzer` rather than the generic framework solver.

## Decision 0218: masked decode matching is a verified pure value operation

**Status:** Accepted and implemented

**Context / Goal**
Instruction and protocol decode needs a compact way to compare fixed and
don't-care bits without exposing a hand-written mask/value pair in every rule.
Expanding the operation entirely in Python would lose its semantic identity
before ACIR verification and prevent later compiler sharing or optimization;
importing general ASL patterns would expand the language far beyond the current
decode requirement.

**Decision (strong constraint)**
- Agentic Circuit adds one pure function:
  `ac.matches(value, "10x1") -> bool`. The value must have `BitsType(N)` with
  `1 <= N <= 64`. The pattern must be an AST string literal of exactly `N`
  lowercase `0`, `1`, or `x` characters in MSB-first order. `x` is a compile-
  time don't-care bit and does not introduce runtime X/Z semantics.
- No method family, pattern object, marker, hardware type, runtime pattern,
  alternation, capture, Python `match/case`, or general ASL pattern matching is
  added. The existing pyCircuit extended bitmask syntax remains a separate
  frontend policy.
- Parsing is implemented once in `pycircuit-semantic-core`. The shared parser
  accepts an explicit policy flag: pyCircuit retains its existing extended
  grammar, while Agentic calls it with `extended=False`.
- The frontend emits one first-class pure operation:
  `ac.var.matches %input mask M value V`. ACIR requires an integer
  `!ac.var<i1..i64>` input, an exact `!ac.var<i1>` result, in-width UI64 mask
  and value attributes, and `V & ~M == 0`.
- `ACDataFlowAnalyzer` evaluates constant inputs exactly, proves a zero mask
  always true, and otherwise returns the boolean domain. This fact remains an
  analysis result and is not serialized as a frontend marker.
- QueueGraph retains expression kind `masked_match`. Its mask/value are
  canonical exact-width lowercase hexadecimal strings, avoiding JSON precision
  loss for bit 63. Its independent verifier repeats arity, type, canonical
  spelling, width, and `value`-within-mask checks.
- Generated gfsim evaluates `(input & mask) == value`. QueueGraph-to-PYC emits
  only existing vendor-neutral `pyc.constant`, `pyc.and`, and
  `pyc.cmp {predicate = "eq"}`; no new
  backend-only PYC or RTL semantic primitive is introduced.
- Contract epoch remains `0.5`: this is an additive pure-value capability for
  syntax and ACIR operations that were previously rejected.

**Verification**
- Semantic-core tests preserve the existing pyCircuit extended grammar while
  proving Agentic basic-policy rejection. Frontend tests cover exact `10x1` to
  mask/value `13/9`, literal-only use, width/type errors, forbidden extended
  characters, deterministic public coverage, and the marker-free API.
- ACIR parser/printer and negative lit cover i1 through i64, wildcard match,
  result/input errors, out-of-width attributes, and value bits outside mask.
- `ACDataFlowAnalyzer` tests cover constant true, constant false, wildcard
  true, and dynamic boolean facts.
- QueueGraph preserves a u64 bit-63/bit-0 mask as canonical hex, rejects forged
  metadata, compiles and executes generated gfsim for true and false cases,
  and lowers PYC to constant/and/eq.
- The public masked decode example runs through generated PYC C++ and Verilator
  with identical cycle/value output for matching and non-matching opcodes. The
  legacy direct Python gfsim adapter also compiles and executes both outcomes.

**Source**
- PTO-ISA/pyCircuit issue #39.
- User objective (2026-09-06): keep decode authoring compact and Pythonic while
  MLIR owns semantic verification and every admitted backend remains aligned.

## Decision 0219: hard-break ACIR/PYC primitive cleanup precedes vector removal

**Status:** Accepted and implemented

**Context / Goal**
The merged Agentic value/rule flow exposed dormant ACIR surface, phase-0 string
proofs, duplicated PYC semantic spellings, immediate-only shift operations, and
the absence of an exact PYC inventory. These increase compiler paths without
adding expressive power and make the later DavinciOO consumer depend on
ambiguous or unverified primitives. Issue #42 separately owns removal of the
misinterpreted first-class PYC vector IR; this decision must not implement that
work early.

**Decision (strong constraint)**
- Remove `ac.module.generated` and its Python decorator/export. Preserve only
  ordinary and external module declarations.
- Remove raw `ac.record.create/get/with` and
  `ac.packet.serialize/deserialize`, including ProcessStatePlan roles and
  payload machinery. Canonical aggregate values remain `ac.var.tuple/array/
  element/get/with/extract/concat/insert` over struct, enum, builtin tuple, and
  `!ac.value_array`.
- Remove dormant ACIR `!ac.address`, `!ac.duration`, `!ac.rate`, `!ac.map`,
  `!ac.set`, `!ac.union`, `!ac.optional`, `!ac.list`, and `!ac.vector`, plus
  `ac.union`. `!ac.value_array` and topology `!ac.array` remain distinct.
  Generic verifier/layout branches do not count as a production consumer.
- Complete Decision 0199: typed/SSA summaries, state footprints, activation
  resources, transaction resources, and structural firing operations become
  the sole rule contract. Delete `guard`/`functional_guard`, `checks`,
  `handshake`, `schedule`, `effects`, and their `ac.rule.*` string attributes;
  do not preserve compatibility aliases or dual writing.
- Remove single-value type/value-fact marker enums when their transient marker
  role has no remaining state space. Arbitration retains resource and lexical
  priority records, but a single-value arbitration-kind wrapper is removed
  unless implementation proves an additional real policy.
- Rename canonical `pyc.mux` to `pyc.select`. Consolidate `pyc.eq`, `pyc.ult`,
  and `pyc.slt` into one `pyc.cmp` operation with a closed predicate attribute.
  Producers, folders, passes, C++/Verilog emitters, Agentic lowering, tests, and
  docs switch atomically; old spellings are unregistered and unparsed.
- Delete `pyc.shli`, `pyc.lshri`, and `pyc.ashri`. Constant amounts are ordinary
  SSA constants feeding `pyc.shl`, `pyc.lshr`, or `pyc.ashr`. ACIR
  `ac.var.shr` is unsigned logical and lowers to `pyc.lshr`; signed arithmetic
  right shift remains the distinct `pyc.ashr` operation.
- Add a machine-readable normative PYC inventory covering every registered op
  and type, its stage/status, producers, verifier/folder/canonicalization,
  ACIR-to-PYC mapping, emitters, RTL selection, positive/negative MLIR, and E2E
  consumers. A read-only gate derives an exact ledger and rejects ODS,
  registration, inventory, or coverage drift.
- The seven existing PYC vector-specific operations remain unchanged in this
  issue but are inventoried as pending removal under issue #42. #41 does not
  remove `AnyVectorOfAnyRank`, vector emitters/runtime, `pyc.v_*`, or vector
  tests. #42 must hard-break and scalarize them before canonical PYC.
- Contract epoch remains `0.5` for Agentic ACIR. PYC is hard-break-only and
  intentionally provides no parser aliases for deleted or renamed operations.

**Required ordering**
1. Remove dormant ACIR surface and synchronize its exact inventory.
2. Delete legacy rule strings after typed verifier consumers are complete.
3. Rename/consolidate scalar PYC operations and shifts verifier-first.
4. Land the exact PYC inventory gate and update normative documentation.
5. Run closure and merge issue #41 before beginning issue #42.

**Verification**
- ODS and parser hard-break tests reject every removed ACIR declaration/type,
  legacy rule/firing string attribute, and removed PYC spelling.
- Canonical PYC uses `pyc.select`, closed-predicate `pyc.cmp`, and SSA-only
  shifts in the Python DSL, Agentic lowering, transforms, C++ emitter, and
  Verilog emitter. A mismatched select result and unknown compare predicate
  both fail in the dialect verifier.
- The generated PYC inventory matches exactly 48 registered operations and two
  types, records the qualified RTL-selection boundary, and assigns only the
  seven unchanged `pyc.v_*` operations to issue #42.
- Current-checkout validation passes 151 Python/PYC/vector tests with two
  optional skips, 32 Queue/gfsim integration tests with one optional skip, and
  all 18 ACIR-to-PYC C++/Verilog integration cases.
- Closure evidence is archived under
  `docs/gates/logs/20260907-issue41-primitive-cleanup/`.

**Source**
- PTO-ISA/pyCircuit issue #41 and its 2026-09-06 vector-scope clarification.
- User direction (2026-09-07): maintain the repository with administrator
  merges, keep `enforce_admins=true` outside the minimal merge window, and
  finish #39, #41, #42, and #44 serially before DavinciOO implementation.
