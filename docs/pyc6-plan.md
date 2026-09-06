# pyCircuit 6 Evolution Plan

This plan turns the accepted contracts in
[`pyc6-decisions.md`](rfcs/pyc6-decisions.md) into reviewable work. The V6
language specification is the product contract; the decision corpus records why
that contract exists.

## Ground rules

- Treat `docs/v6_PyCircuit_Specification.md` as the language source of truth.
- Treat `docs/rfcs/pyc6-decisions.md` as the semantic decision source of truth.
- Preserve the CycleAwareSignal model defined by Decision 0148.
- Add or tighten MLIR verification before changing semantics.
- Do not implement semantic fixes in only one backend.
- Build and test from the current worktree.
- Reference affected decision IDs and archive reviewable semantic evidence under
  `docs/gates/logs/<run-id>/`.
- Use `libpyc6_runtime`, `.pyctrace` schema magic `PYC6TRC3`, and
  `run_semantic_regressions_v6.sh` for active products and gates. Preserve only
  historical files under `docs/gates/logs/` as immutable evidence.

## Baseline inherited by V6

The earlier closure evidence remains the verified baseline for Decisions
0001–0147. It covers static-hardware legality, backend equivalence, observation
points, reset and trace semantics, structured interfaces, DFX, incremental
builds, and cosimulation. The active status index is
`docs/gates/decision_status_v6.md`; its older rows retain their original
evidence paths.

V6 adds Decision 0148: the global CycleAwareSignal design is the primary
authoring model. This supersedes Decision 0010 without invalidating the
unrelated earlier closure evidence.

Decision 0150 adds Agentic Circuit as an independent upper-level ACIR and
frontend in this repository. Its synthesizable path converges on verified PYC
IR and the pyCircuit 6 backends; its ACSim/gfsim architecture-simulation path
remains distinct.

## Milestones

### Documentation and governance convergence

**Goal:** Present one current language and one repository authority.

- [x] Make the V6 specification, tutorial, and software architecture the
  product-facing documentation set.
- [x] Supersede Decision 0010 with Decision 0148.
- [x] Remove the duplicate prior-version specification from current navigation.
- [x] Document PTO-ISA/pyCircuit as upstream and LinxISA/pyCircuit as a
  downstream fork.
- [x] Keep automation, issue templates, release metadata, and repository rules
  aligned with the upstream/fork ownership model.

### Public API convergence

**Goal:** Remove versioned implementation vocabulary from the supported API
without removing cycle-aware semantics.

- [x] Make CycleAwareSignal examples and API references consistent across the
  README, tutorial, and reference pages.
- [x] Rename prior-version-labelled modules, comments, diagnostics, and tests where
  the name is not an ABI or serialized compatibility contract.
- [x] Keep `CycleAwareCircuit`, `CycleAwareDomain`, `CycleAwareSignal`,
  `CycleAwareTb`, and `compile_cycle_aware()` supported as defined by the V6
  specification.
- [x] Reject removed compatibility APIs instead of silently accepting them.

### Post-6.0 cycle-aware hardening backlog

Decision 0148's public V6 contract is implemented and verified by the focused
V6 tests plus the examples, simulation, and semantic lanes archived under
`docs/gates/logs/pyc6-unification/`. The items below expand coverage beyond the
6.0 migration baseline and remain follow-up hardening work.

- [ ] Verify cycle provenance across `domain.call()` and `pyc.instance`
  boundaries.
- [x] Verify automatic delay insertion for mixed-cycle arithmetic, comparison,
  mux, vector, and structured signals.
- [x] Verify `domain.signal()` inference for combinational, single-stage, and
  multi-stage assignments.
- [ ] Verify invalid backward-cycle assignments fail with source-located
  diagnostics.
- [x] Compare C++ and Verilator results at TICK-OBS and XFER-OBS.

### Repository and release closure

**Goal:** Make PTO-ISA/pyCircuit the only release and governance authority.

- [x] Require protected review and required checks on the upstream default
  branch.
- [x] Publish releases and packages only from PTO-ISA/pyCircuit.
- [x] Remove consumer-owned Linx, Janus, XiangShan, QEMU-comparison, and board
  designs/tools from the framework tree; consumers pin pyCircuit externally.
- [x] Ensure package metadata, documentation URLs, badges, and source links name
  PTO-ISA/pyCircuit.

### Agentic Circuit consolidation

**Goal:** Make this repository the only development and release authority for
ACIR, Agentic Circuit, and pyCircuit without weakening the CycleAwareSignal or
PYC semantic contracts.

#### Issue #39: static value types and decode-oriented bitfields

- [x] Record Decision 0211 for `bits[N]`, bool/u1, signedness, layout identity,
  width limits, bitfield ordering, and contract-epoch handling.
- [x] Add the neutral `pycircuit-semantic-core` package so pyCircuit and
  Agentic Circuit use one immutable bitfield validation/fingerprint
  implementation without merging public namespaces.
- [x] Close the first bit-operation slice through static `bits[N]`, half-open
  extract, MSB-first concat, immutable insert, ACIR verification, QueueGraph,
  gfsim, and PYC.
- [x] Add Agentic `BitfieldSpec`, closed `(msb, lsb)` fields, overlapping read
  views, disjoint immutable updates, stable schema fingerprints, and
  field-qualified ACIR provenance verification.
- [x] Execute a 32-bit u3/u5/u17 overlapping-view decode through generated
  gfsim and compare a scalar bitfield transform cycle-for-cycle through PYC
  C++ and Verilator.
- [x] Add Decision 0212's immutable recursive descriptor covering Bool, Bits,
  Enum, Struct, Tuple, and Array; migrate scalar annotation and flat-struct
  payload discovery to descriptors before MLIR rendering.
- [x] Separate fixed payload arrays from topology collections in ACIR:
  `!ac.value_array<N x T>` is recursively immutable, builtin tuple payloads
  require immutable elements, and existing `!ac.array` remains Queue/Var
  topology only.
- [x] Complete Decision 0216 by replacing QueueProgram and expression-lowering
  MLIR type strings with descriptor identities and keep string conversion at
  the ACIR printer boundary only.
- [ ] Separate logical bool from u1 only with an explicit hard-break decision
  and a coordinated ACPy/ACIR contract-epoch transition.
- [x] Add Decision 0213's nested nominal struct slice with source-order-
  independent resolution, recursive field access/replacement, QueueGraph cycle
  rejection, dependency-ordered gfsim C++ types, and PYC C++/RTL parity.
- [x] Add Decision 0214's standard Python `enum.Enum` slice with contiguous
  declaration-order encoding, canonical `ac.enum`/`ac.var.enum`, nested struct
  fields, equality-only semantics, QueueGraph metadata, compact gfsim enums,
  and PYC C++/RTL parity.
- [x] Add Decision 0215's tuple and fixed-size value-array lowering; keep value
  arrays distinct from persistent Python lists selected to `ac.var` storage.
- [x] Add Constant/FiniteSet/ClosedInterval/Unknown static constraints and use
  them for widths, slices, indices, shifts, loops, and enum exhaustiveness.
- [x] Add verified masked bit matching and a decode example without importing
  general ASL pattern matching.
- [x] Restore the broad PYC integration suite to 13/13: derive the framework
  DavinciOO workload from tracked framework goldens instead of a consumer
  trace, verify two fork delivery-state bits, compile split dependency and
  DavinciOO translation units, and print exact-width gfsim values explicitly.
- [x] Run complete issue #39 closure, prepare `Closes #39`, merge upstream, and
  confirm the issue is closed before producing the consumer pin.

#### Strict cleanup and consumer handoff sequence

- [x] Finish, review, merge, and confirm closure of upstream issue #39. Do not
  begin issue #41 or consumer implementation before this gate.
- [ ] Complete Decision 0219 and upstream issue #41's non-vector primitive
  cleanup:
  - [x] Remove `ac.module.generated`, raw `ac.record.*`/`ac.packet.*`, and the
    dormant ACIR declarations/types named by #41 while preserving canonical
    `ac.var` aggregates, `!ac.value_array`, and topology `!ac.array`.
  - [x] Complete Decision 0199 by deleting legacy rule/firing strings after
    typed/SSA verifiers, footprints, activation, and QueueGraph consumers are
    authoritative.
  - [x] Rename canonical PYC `mux` to `select` and consolidate `eq/ult/slt`
    into `cmp {predicate}` without compatibility aliases.
  - [x] Remove `shli/lshri/ashri`; retain only SSA `shl/lshr/ashr`, with
    Agentic unsigned `shr` lowering to `lshr`.
  - [x] Add an exact machine-readable PYC inventory and generated coverage/
    producer/emitter/consumer ledger; mark unchanged vector ops as #42-owned.
  - [ ] Run #41 closure, admin-merge its independent PR, restore
    `enforce_admins=true`, and confirm #41 closed.
- [x] Delegate issue #41's vector section to issue #42; do not modify PYC
  vector operations, lowering, emitters, runtime, or tests in the #41 change.
- [ ] Then complete upstream issue #42's vector hard break: remove first-class
  PYC vector IR/backend/runtime behavior while retaining ACIR aggregate value
  types and scalarizing them before canonical PYC.
- [ ] Then complete issue #44: allow `ac.jit(system)` to bind structural
  `ac.const` parameters while leaving ordinary typed parameters as inferred
  runtime Queue inputs. Close the framework-owned multi-input/multi-output ROB-
  and CMT-shaped ACIR→gfsim compatibility gates before producing a consumer
  pin, including branch-local consume-and-classify and four-output
  backpressure/state-retention behavior.
- [ ] Only after #39, #41, #42, and #44 are merged and closed, pin that
  pyCircuit revision in the owning consumer repository and begin the DavinciOO
  Core. pyCircuit retains only reusable framework code and gates.

- [x] Import the Agentic Circuit `main` history at
  `756002e2998b11dfe1fed14dc3d63cdad8be694c` with provenance intact.
- [x] Record PTO-ISA's BSD-3-Clause owner direction and the imported main and
  open-PR head objects in `docs/legal/AC-RELICENSE-BSD-3-CLAUSE.md`.
- [x] Migrate the unique work and review disposition from Agentic Circuit PR
  #18 and PR #23 into reviewable pyCircuit changes.
- [x] Integrate the `agentic_circuit` distribution, ACPy/ACIR, ACSim/gfsim,
  and ACIR-to-PYC targets without merging their Python or MLIR namespaces.
- [x] Replace prior-version pyCircuit runtime references with repo-local
  pyCircuit 6 targets and `libpyc6_runtime`.
- [x] Run the ACIR/ACSim verifier and unit lanes, ACIR-to-gfsim execution, and
  ACIR-to-PYC-to-C++/Verilog gates from the consolidated checkout.
- [x] Run the existing pyCircuit 6 API, examples, simulation, and semantic
  closure lanes from the same checkout.
- [x] Attach gate evidence and promote Decision 0150 to
  `implemented-verified` for repository and compiler consolidation.
- [ ] Close Decision 0151 for Agentic Circuit epoch `0.4`: verify the
  one-dimensional zero-initialized single-writer Table through ACPy, Frozen
  ACIR, QueueGraph, and typed gfsim C++, with a stable PYC rejection boundary.
- [x] Close Decision 0152 without changing epoch `0.4`: verify state-driven
  Table updates, match/choose, and committed Queue slots through both typed
  gfsim C++ generators while preserving the same PYC rejection boundary.
- [x] Close Decision 0153 without changing epoch `0.4`: accept a same-Table
  CandidateSet in `table.view(mask)`, retain scalar-index writes, and verify
  atomic uniform masked write/patch through both typed gfsim C++ generators.
- [x] Close Decision 0154 without changing epoch `0.4`: permit multiple Table
  writer endpoints with pairwise-disjoint top-level field sets and merge their
  old-state proposals atomically through both typed gfsim C++ generators.
- [x] Close Decision 0155 without changing epoch `0.4`: lower each authored
  Table match/choose once and reuse its lazy full-Epoch result across all
  direct and native gfsim endpoint consumers.
- [x] Close Decision 0156 without changing epoch `0.4`: add one state-driven
  scalar allocation endpoint whose complete Entry replacement wins over
  same-Entry field writers in direct and native typed gfsim C++.
- [ ] Implement `D-RULE-LOWERING-001`: make `@ac.rule` the simple Python
  scheduling boundary; infer types, effects, checks, handshake, and conflicts
  through staged ACIR passes; preserve incomplete knowledge with typed markers;
  bump the Agentic Circuit contract epoch to `0.5`; and reject every unresolved
  marker before Frozen ACIR topology freeze, hashing, or serialization.
  - [x] Land the epoch `0.5` hard break, remove Python `atomic`/`.firing()`, add
    closed typed marker attributes, and implement the one-input/one-output pure
    rule pipeline through internal `ac.firing`, proven `ac.transform`,
    QueueGraph, gfsim, and the PYC build lane.
  - [x] Land Decision 0162's gfsim transaction substrate: owner-tagged Queue
    and Table prepare/publish/no-fail commit groups, dynamic index/field
    footprints, and four Table-plus-Queue ROB-shaped transition regressions.
  - [x] Land Decision 0163's first stateful rule slice: simple Python Table
    read/complete Entry assignment, firing-local `ac.table.propose`, inferred
    Table-aware handshake/schedule, marker-free `ac.firing`, QueueGraph, and
    grouped gfsim execution with provisional PYC rejection.
  - [x] Land Decision 0167's first multi-Queue slice: accept variadic inputs on
    one pure `@ac.rule`, infer `ready_valid_Nx1` in MLIR, and lower to one typed
    gfsim atomic transform without exposing Queue mechanics in Python.
  - [x] Land Decision 0168's stateful multi-Queue slice: accept heterogeneous
    Queue payloads beside one owner Table, infer `ready_valid_Nx1_table`, and
    generate one variadic gfsim Table transaction with all-resource commit.
  - [x] Land Decision 0169's first variable-analysis slice: use
    `ACDataFlowAnalyzer` over MLIR dataflow to infer orthogonal lifetime/update
    properties and lexical ownership without hardware-named Python annotations
    or markers.
  - [x] Land Decision 0170's generic persistent-variable IR slice:
    `ac.var.decl/read/assign` storage-select to existing committed state before
    rule lowering, preserving one ac.var concept and the grouped gfsim path.
  - [x] Land Decision 0171's inferred external-boundary slice: ordinary typed
    system parameters become internal sources and typed returns become one or
    more internal sinks, with no Python source/sink syntax required.
  - [x] Land Decision 0172's first persistent-list slice: infer fixed shape
    from a normal zero-initialized Python `list`, retain indexed operations in
    the `ac.var` family, and storage-select to touched-entry Table updates.
  - [ ] Replace phase-one string rule proofs with typed guards, selected output
    presence, Queue effects, state footprints, and path-qualified alternatives.
  - [x] Land Decision 0173's stateful arbitration substrate: Work computes an
    immutable candidate only, while stable Arbitrate order performs the atomic
    prepare/publish step independently of Work traversal order.
  - [x] Land Decision 0174's first shared-state slice: `ACDataFlowAnalyzer`
    emits ordered read/write footprints, MLIR freezes lexical rule priority,
    and multiple whole-entry rules may share one inferred persistent list.
  - [x] Land Decision 0176's multi-owner transaction slice: one rule may update
    heterogeneous scalar/list state owners, and QueueGraph/gfsim commits every
    owner plus selected Queues in one group using allocation-free candidate
    write slots.
  - [ ] Generalize shared-state analysis from whole-entry proposals to
    path-qualified field/index footprints and conditional proposals.
  - [x] Land Decision 0175's first guarded/arity slice: support consume-only
    input rules, state-driven zero-input rules, a typed single `if` condition,
    and inferred `Nx0`/`0x1` handshakes through generated gfsim.
  - [ ] Generalize the single guarded block into CFG joins and path-qualified
    effects; require every selected path to close one atomic
    prepare/publish/no-fail commit group.
  - [ ] Extend 0/1 rule results to optional and multiple selected results, and
    derive backpressure from the chosen output presence set.
  - [x] Generalize one rule activation to propose updates to multiple lexical
    persistent variables, including fixed lists with dynamic indices.
  - [ ] Extend `ACDataFlowAnalyzer` footprints with path predicates and
    read/write conflict classes, then build explicit arbitration IR for each
    owner-local compatible candidate set.
  - [ ] Preserve module specializations in QueueGraph/gfsim so repeated module
    instances share one implementation instead of flattening generated code.
    - [x] Admit hierarchy-preserving QueueGraph freeze IR using existing
      `ac.system`, `ac.module`, `ac.instance`, and `ac.scope`, and seal each
      definition/instance with compiler-derived fingerprints.
    - [x] Extract specialization-keyed QueueGraph definitions and instance
      bindings without copying module bodies into the root plan.
    - [x] Generate one typed gfsim implementation class for the first pure
      one-input/one-output transform specialization and verify two instances
      bind independent Queues to that class and execute independently.
    - [x] Give the first single-rule stateful specialization one implementation
      class with independently constructed persistent Table state per instance.
    - [x] Generalize one-owner specialization classes to multiple firing rules
      and arbitrary direct interface Queue arity while preserving lexical
      arbitration.
    - [x] Generalize a reusable firing to heterogeneous multi-owner state using
      one per-instance `QueueStateTransition` commit group.
    - [x] Combine multi-rule and multi-owner specialization classes while
      preserving per-rule owner subsets and lexical arbitration.
    - [x] Consolidate the pure, single-owner, multi-owner, and combined codegen
      branches behind one typed stateful-specialization emitter before adding
      more module shapes.
      - [x] Centralize structured field-merge policy generation and diagnostics
        across all stateful specialization shapes.
      - [x] Centralize transition policy/class/member emission and delete the
        remaining shape-specific branches.
    - [x] Preserve direct nested specialization wrappers with child-before-
      parent plan/codegen order and recursive dense runtime-ID partitions.
    - [x] Preserve the first mixed local-transform plus nested-child module with
      one independently owned internal Queue per parent instance.
    - [ ] Generalize mixed modules to arbitrary internal Queue graphs, multiple
      local blocks, and local state combined with child instances.
    - [x] Lower the first pure typed 1x1 `@ac.module` definition and ordinary
      Python calls into structured QueueGraph without frontend markers or
      hardware-specific authoring objects.
    - [x] Lower a direct nested Python module return into parent/child
      specialization structure and recursive gfsim reuse.
    - [x] Lower one zero-initialized scalar lexical variable in a typed Python
      module through `ac.var` analysis, MLIR storage selection, and a reused
      gfsim specialization with independent state per instance.
    - [x] Generalize the stateful Python module slice to multiple scalar
      lexical variables with serial assignment semantics and one inferred
      multi-owner atomic commit group.
    - [x] Reuse the system rule/variable body parser and QueueProgram renderer
      for direct-interface `@ac.module` bodies with arbitrary Queue arity and
      multiple ordinary rule calls.
    - [x] Move the four-rule, five-owner circular ROB behind a reusable typed
      3-input/2-output module and prove two placements share one implementation
      class while retaining independent state.
    - [ ] Extend Python module lowering to arbitrary internal Queue graphs,
      conditional state updates, static parameters, and compiler-inferred
      fanout for repeated values.
  - [x] Land Decision 0177's real circular ROB example covering allocation,
    completion, in-order retire or oldest-ready issue, full/empty backpressure,
    wraparound, stale-generation rejection, flush/recovery, and atomic state
    plus Queue effects.
  - [x] Land Decision 0189's reusable circular ROB module using the same four
    rules and five lexical owners behind a 3-input/2-output typed interface.
  - [ ] Replace QueueGraph's per-tick full `dispatch_rows()` scan with generated
    incremental activation while preserving an explicit scan-mode reference.
    - [x] Add Decision 0190's first exact Queue/Table-to-transaction-closure
      adjacency and initial zero-input frontier to QueueGraph plans.
    - [x] Bind direct leaf-specialization edges through per-instance dense IDs
      and emit canonical activation offsets, targets, and initial Work IDs.
    - [x] Prove the first scan/activation result equivalence on the reusable
      ROB; 32 dispatch rows require 134 scheduled Work calls instead of the
      256 calls in an eight-tick full scan reference.
    - [x] Add Decision 0191's enum-typed MLIR activation sources, transaction
      resources, and initial-rule facts; make QueueGraph consume this evidence
      for rule-backed blocks.
    - [x] Generate `offer_<input>(SimSystem&, value)` adapters and initial-Work
      scheduling so external input proposals enter activation automatically.
    - [x] Recursively bind activation through nested wrappers and parent-local
      internal Queues without flattening reusable specialization classes.
    - [x] Add Decision 0193's separate Work-closure CSR, owner-first/resource-
      second arbitration, global Probe barrier, and external-Xfer frontier.
    - [x] Move generated input offers from Queue-as-Work scheduling to explicit
      external Xfer enrollment; the ROB scenario now uses 40 Work calls.
    - [x] Add Decision 0194's compiler-owned root Queue results and generated
      external dequeue adapters without changing the Python authoring surface.
    - [x] Add Decision 0195's semantic-change filter: equal final Table values
      still commit their atomic transaction but do not propagate activation;
      changed values retain the existing wake graph.
    - [x] Add Decision 0196's full reusable-ROB equivalence gate: compare every
      tick's ten Queue images, both instances' scalar/entry state, exact result
      visibility, and validated commit timeline; lock 1769 scan Work calls
      versus 182 incremental Work calls for the complete scenario.
  - [ ] Replace string guard/effect proofs with typed path-qualified CFG facts,
    output presence, conflict classes, and explicit arbitration IR before
    general transactional fanout.
    - [x] Add Decision 0199's phase-0 typed-summary bridge for the current
      single-condition 0/1-output subset: closed guard/check/effect/presence/
      state-access/schedule/arbitration-membership enums, pass derivation,
      independent verifier reconstruction, and unchanged ROB/ISQ execution.
    - [x] Add Decision 0200's SSA `!ac.var<i1>` presence to each output and
      state proposal in the current single-condition, 0/1-output subset;
      preserve it through Firing and QueueGraph, and verify it independently
      before admitting CFG joins or multiple selected outputs.
    - [x] Add Decision 0201's conditional-effect slice: recognize a serial
      Python early `return` as unconditional input consumption followed by
      optional state effects; carry a distinct SSA presence through `ac.var`
      storage selection, Firing, QueueGraph, and gfsim without weakening the
      existing blocking semantics of a trailing rule guard.
    - [x] Add Decision 0202's exact top-level `TableGet` snapshot reservations
      for conditional-effect predicates; infer read-only scalar state, preserve
      typed proof through QueueGraph/gfsim, and remove the ROB completion
      self-assignment previously used to force epoch serialization.
    - [x] Add Decision 0203's candidate/output snapshot roots and exact
      `table.match` dependency sets: emit compiler-owned
      `ac.state.snapshot_set`, accumulate its mask during the original scan,
      and cover ISQ same-tag clear plus continuous unrelated-readiness stress.
    - [x] Add Decision 0204's exact choose-key dependency sets: use the
      canonical `table.choose` index result as evaluation provenance and
      accumulate foreign-state indices only for candidates whose key executes.
    - [x] Add Decision 0205's field-qualified snapshot relation: propagate
      direct `ac.var.get` field use into snapshot proof and encode exact
      `(entry, field)` pairs through QueueGraph and gfsim without allocation.
    - [x] Add Decision 0206's serial early-return guard chains: combine
      contiguous pre-effect returns into one SSA effect presence while keeping
      input acceptance unconditional and Python control flow ordinary.
    - [x] Add Decision 0207's first branch-local effect join: lower one Python
      `if/else` to a verifier-proven complementary presence pair over distinct
      lexical state owners and one atomic runtime candidate.
    - [x] Add Decision 0208's scalar same-owner join: introduce compiler-owned
      `ac.var.select`, produce one state proposal, and preserve gfsim/PYC mux
      parity without adding a Python primitive.
    - [x] Add Decision 0209's indexed same-owner join: select both branch index
      and value, retain full-domain safety, and emit one dynamic state proposal.
    - [x] Add Decision 0210's first optional selected output: keep input/state
      candidate true, carry independent SSA output presence, and apply output
      backpressure only when that output is selected.
    - [ ] Extend joins to multiple selected outputs, nested/multi-block CFG,
      multi-input branches, and more-than-64-bit entry/field relations without
      weakening provenance or module reuse.
    - [ ] Derive pairwise conflict relations/classes and explicit contender
      arbitration from satisfiable path-qualified state footprints.
  - [x] Add Decision 0197's compiler-recognized `ac.find` over an ordinary
    persistent list, storage-neutral `ac.var.match/choose`, read-only state
    activation, and a reusable oldest-ready ISQ with persistent ready lookup,
    same-epoch lost-wakeup coverage, tag clear/reuse, output backpressure, full
    input retention, and two independent placements sharing one class.
  - [x] Add Decision 0198's ISQ scan/incremental lockstep gate: compare every
    boundary Queue, both entry/readiness images, exact output visibility, and
    validated commit timeline; lock 952 scan Work calls versus 129 incremental
    Work calls for the complete lost-wakeup/backpressure/tag-reuse scenario.
  - [ ] Run focused frontend/ACIR/gfsim gates after every slice, then archive
    full AC G0/G1/G2 and pyCircuit semantic closure for the ROB/ISQ result.
- [x] Land Decision 0161's first qualified primitive vertical slice:
  vendor-neutral `pyc.priority_encode`, backend-only `pyc.rtl.comb`,
  deterministic digest-verified BSD RTL selection, parameterized pyCircuit and
  Agentic Python APIs, QueueGraph-to-PYC, and gfsim SimQueue execution.
- [x] Land Decision 0164's qualified `pyc.popcount` slice: structural and
  Cycle-Aware Python APIs, semantic PYC/ACIR lowering, C++ and gfsim reference
  behavior, BSD-3-Clause selected RTL, and digest-closed manifest evidence.
- [x] Land Decision 0165's leading-zero semantic slice: define
  all-zero as `N`, preserve exact result width and cycle, lower Agentic ACIR to
  semantic PYC/gfsim, and select repository-owned balanced BSD RTL.
- [x] Generalize Decision 0166 into one parameterized zero-count family:
  leading/trailing remain simple Python helpers, while ACIR/PYC, gfsim, catalog
  selection, and the balanced BSD RTL share one static `direction` contract.
- [ ] Normalize the remaining PR #29 catalog into semantic families. Admit
  combinational families only after per-family C++/RTL/Agentic parity; keep
  stateful, handshake, memory, and CDC families blocked on the inferred
  prepare/publish/no-fail commit contract and circular ROB evidence.
- [ ] After the internal AC G0/G1/G2 and pyCircuit closure passes, disable the
  old repository's publishing/CI authority and make it private with only
  `zhoubot` as a direct repository collaborator.

## Gate mapping

Use the minimum applicable lanes from
[`testing-and-gates.md`](development/testing-and-gates.md).

| Change | Required evidence |
| --- | --- |
| Documentation or governance | changed-file pre-commit checks, API hygiene, `mkdocs build` |
| Cycle-aware frontend or inference | unit tests, API hygiene, examples, semantic regressions |
| MLIR semantics or legality | examples, normal and nightly simulations, semantic regressions, strict decision status |
| C++ or Verilog behavior | both simulation lanes and backend-equivalence evidence |
| Consumer compatibility | Run in the owning consumer repository against a pinned pyCircuit revision |
| ACIR or Agentic Circuit integration | ACIR/ACSim verifier and unit lanes, ACIR-to-gfsim, ACIR-to-PYC-to-C++/Verilog, plus pyCircuit examples, simulations, and semantic regressions |

Use one `PYC_GATE_RUN_ID` for related semantic lanes. Record skipped gates and
their risk in the pull request.

## Completion criteria

The pyCircuit 6 transition is complete when:

- current docs contain no prior-version label as the active product language;
- Decision 0148 has focused tests and cross-backend evidence;
- supported examples use the V6 CycleAwareSignal contract;
- repository metadata and release automation point only to PTO-ISA/pyCircuit;
- the LinxISA repository is maintained only as a framework-compatibility fork,
  while consumer designs and tools remain out of tree; and
- Decision 0150 has archived AC and PYC closure evidence, and the retired
  Agentic Circuit repository has no publishing or CI authority; and
- all required gates pass from a clean worktree.
