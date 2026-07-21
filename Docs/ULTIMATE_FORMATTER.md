# Ultimate Blueprint Graph Formatter

## Executive decision

The selected architecture is a deterministic, native C++ Blueprint/K2 layout core plus a separate,
conservative native wire router. It ships with no Java, JavaScript, .NET, GPL, LGPL, or EPL layout
dependency. The design borrows proven ideas from ELK Layered, Graphviz/dot, Sugiyama-style layered
drawing, and object-avoiding connector routers, but owns the Unreal-specific semantic model,
transactions, geometry capture, and topology safety.

That choice is deliberate. ELK is the closest general-purpose match for port-aware layered layout,
and libavoid is the closest focused match for connector routing, but neither can be dropped into an
Unreal Editor plugin without a runtime, integration, distribution, and licensing cost. A native core
also lets execution pins, data providers, comments, Blueprint cycles, grid alignment, Slate geometry,
and `UK2Node_Knot` ownership be first-class concepts instead of lossy annotations around a generic
graph library.

This document distinguishes three things:

- **Implemented** means the behavior exists in this fork's K2 path.
- **Validation target** means the invariant is required and has some automated or manual coverage,
  but should not be read as a proof for every Blueprint and custom graph policy.
- **Roadmap** means the feature is part of the proposed “god mode” formatter, not a current claim.

The short version is: the current K2 formatter is a large architectural improvement over the legacy
generic path, while the final step from “good deterministic formatter” to “human-quality global
optimizer” still requires richer crossing-driven routing, incremental stability, and a representative
visual regression corpus.

## What failed in the legacy formatter

The observed failure mode was deceptive: Graph Formatter still ran and moved nodes, but their order,
spacing, and wiring no longer matched the author's semantics. There is not enough evidence to name one
engine commit as the sole regression. The most defensible diagnosis is that Unreal Engine UI changes
exposed a brittle integration while several independent algorithm defects were already present.

### Likely engine-change exposure

The legacy implementation depended on private Slate member access and widget-lifecycle assumptions:

- Pointer/member-access tricks reached private graph-panel state such as `ZoomLevels` and the current
  LOD.
- Similar access reached graph-node `UserSize` rather than consuming a public geometry snapshot.
- The formatter manually ticked graph widgets to try to force sizes and pin positions into existence.
- Layout therefore depended on which widgets happened to be constructed, visible, cached, and at a
  particular LOD during the command.

Those assumptions are fragile across Unreal versions. UE 5.7's public APIs and Slate lifecycle differ
enough from the environment in which the original plugin was written that stale or missing geometry can
look like a successful formatting pass: the algorithm receives plausible objects, but wrong widths,
pin anchors, or graph-panel state. This is a likely engine-change exposure, not a proven single-cause
regression.

The K2 replacement captures public geometry after a normal editor tick. It does not tick Slate
manually, change zoom, or read private widget fields. Persisted positions, trustworthy desired or
persisted node sizes, and deterministic pin offsets cover safe partial observations. If a node has no
measurable or persisted size, the coordinator requests a normal panel update, retries once, records
diagnostics, and aborts without mutation if geometry is still incomplete.

### Independent latent algorithm defects

The original generic path also contained defects that do not require an engine change to explain poor
ordering:

- `MaxLayerNodes` artificially wrapped ranks, breaking the meaning of a dependency layer.
- `TSet`/container iteration leaked into layout decisions, so equivalent insertion orders could produce
  different drawings.
- A crossing baseline initialized to `INT_MAX` prevented meaningful before/after comparisons.
- Pin indices and conflict iterators could become stale while the algorithm was reordering nodes.
- Median selection was mathematically wrong for some even and sparse neighbor sets.
- A random GUID fallback made tie-breaking and repeated runs unstable.
- Execution and data edges were mixed into one undifferentiated graph, allowing data convenience to
  distort control flow.

Repairing these defects improves the generic formatter, but it does not give a generic graph model the
semantics of a Blueprint. The durable fix is the architectural split described below.

## Requirements, in priority order

The formatter treats readability as a constrained optimization problem. Lower-priority aesthetics must
never buy their score by violating a higher-priority invariant.

1. Preserve graph topology and Blueprint meaning.
2. Never overlap semantic nodes; keep comments and partial selections safe.
3. Make execution flow obvious, predominantly left-to-right, and preserve branch-pin order.
4. Keep primary execution connections straight through pin alignment where possible.
5. Place pure/data providers near the consumer inputs they feed without perturbing execution ranks.
6. Keep wires and generated knot boxes away from nodes.
7. Reduce crossings, coincident wires, unnecessary bends, and wires hidden under nodes.
8. Use generous, consistent spacing and graph-grid alignment.
9. Minimize unnecessary movement and produce the same answer on a repeated run.
10. Remain responsive and provide a safe unchanged result when a bounded search cannot find a route.

“Compact” is intentionally last. A larger graph that reads like a careful human layout is preferable to
a dense graph with ambiguous wires.

## Ecosystem and license decision matrix

Licenses below describe the linked upstream projects as of this review; they are not legal advice. Any
future vendoring or binary distribution still requires project legal review.

| Candidate | Relevant strengths | Integration and license | Decision |
|---|---|---|---|
| [ELK Layered](https://eclipse.dev/elk/reference/algorithms/org-eclipse-elk-layered.html) | The strongest conceptual fit: layered directed layout, explicit ports, port order/side constraints, compound graphs, crossing minimization, and straight/orthogonal/spline routing. Its [port constraints](https://eclipse.dev/elk/reference/options/org-eclipse-elk-portConstraints.html) and [model-order constraints](https://eclipse.dev/elk/blog/posts/2023/23-01-09-constraining-the-model.html) map well to Blueprint pins. | The [ELK repository](https://github.com/eclipse-elk/elk) is Java under EPL 2.0. Shipping it means embedding/launching a JVM, using a transpiled JS build, or maintaining a native port, plus EPL notices and source obligations for modifications. It still would not understand Unreal transactions or K2 topology. | **Reference design, not a shipping dependency.** Use its phase structure and constraint vocabulary as an oracle for future improvements. |
| [libavoid](https://www.adaptagrams.org/documentation/libavoid.html) | Cross-platform C++ object-avoiding orthogonal/polyline connector routing, incremental diagram-editor use, crossing/segment penalties, and mature routing research. | The [Adaptagrams repository](https://github.com/mjwybrow/adaptagrams) is LGPL 2.1-or-later and dual-licensed commercially. It is a router, not a complete semantic node-placement engine, and its repository has no formal releases. Static Unreal plugin distribution needs careful LGPL compliance or a commercial license. | **Best optional future routing backend**, but only behind an adapter and only after licensing/build evaluation. Not required by the current native router. |
| [Blueprint Assist](https://blueprintassist.github.io/miscellaneous/settings/) | Native Unreal integration, strong workflow polish, auto-formatting, extensive settings, and useful [visual formatting examples](https://blueprintassist.github.io/miscellaneous/formatting-examples/). It is the closest product-level usability benchmark. | It is a paid proprietary Fab/Marketplace product, not an open-source layout library. Public documentation does not grant reusable source rights. Its own listing recommends a separate comment-sizing plugin. | **UX and output-quality benchmark only.** It cannot be the foundation of this MIT plugin. |
| [Graphviz/dot](https://graphviz.org/docs/layouts/dot/) | Proven hierarchical ranking, crossing reduction, clusters, ports in several representations, and node-avoiding spline output. Native C libraries are widely deployed. | Current Graphviz is [EPL 2.0](https://graphviz.org/license/). Its [orthogonal spline mode](https://graphviz.org/docs/attrs/splines/) explicitly does not handle ports in `dot`, which is a major K2 mismatch. Adapting DOT output back to exact Slate pin anchors and UE transactions would remain substantial. | **Algorithm reference and possible offline comparison tool**, not the selected in-editor engine. |
| [OGDF](https://ogdf.github.io/doc/ogdf/group__graph-drawing.html) | Very broad native C++ graph-drawing toolkit: Sugiyama, planarization, orthogonal, clustered, and force-directed algorithms. | OGDF is distributed under GPL v2 or v3 according to its [official license page](https://www.ogdf.uni-osnabrueck.de/license/). That is not compatible with the current goal of distributing this plugin as an MIT editor plugin without broader copyleft obligations or a separate agreement. Its generic ports still need a K2 adapter. | **Rejected for the shipping plugin on license grounds.** Useful research reference. |
| [MSAGL](https://github.com/microsoft/automatic-graph-layout) | High-quality layered layout, obstacle-aware spline/rectilinear routing, large-graph work, and a permissive MIT license. | The primary implementation is C#/.NET. Unreal Editor does not provide the required managed hosting/deployment surface by default. A native port would become a long-lived fork, while an out-of-process helper would weaken transactions, portability, and interactive latency. | **Rejected for runtime mismatch**, despite the friendly license and strong algorithms. |
| [dagre](https://github.com/dagrejs/dagre) / [d3-dag](https://github.com/erikbrinkman/d3-dag) | Lightweight MIT-licensed JavaScript/TypeScript layered layout, easy experimentation, familiar Sugiyama phases. | Requires a JS runtime or port and lacks the complete combination of arbitrary fixed Blueprint ports, compound comment behavior, K2 cycles, native object-aware routing, and Unreal transactional integration. | **Useful prototype/reference, insufficient as the ultimate engine.** |
| This fork's native K2 core | Direct access to K2 pin semantics, public Slate geometry, Unreal transactions, comments, graph grid, `UK2Node_Knot`, and existing MIT distribution. No foreign runtime or ABI. | More algorithm work is owned by this project, including tests and maintenance across UE versions. It must resist gradually becoming an untestable editor monolith. | **Selected.** Keep the core engine-independent and the Unreal adapter thin. |

## Selected native architecture

The architecture separates observation, pure layout, routing, and mutation:

```text
public Unreal/Slate geometry
            |
            v
  immutable K2 semantic snapshot -----> validate or return unchanged
            |
            v
  engine-independent layout core -----> deterministic layout plan
            |
            v
   node/comment plan + immutable routing inputs
            |
            v
             one guarded transaction
            /                     \
           v                       v
 apply node/comments      plan then materialize each wire
            \                     /
             +---------+---------+
                       v
          one Blueprint mark + graph notify
```

### Component responsibilities

| Component | Responsibility | Must not do |
|---|---|---|
| `GraphGeometrySnapshot` | Observe public node/pin geometry and record the source and quality of every measurement. | Tick widgets, change the view, or reach private Slate members. |
| `K2GraphFormatter` | Adapt K2 nodes/pins into a semantic snapshot, coordinate comments and fixed obstacles, validate the plan, and apply it. | Hide ordering logic inside editor callbacks or mutate while planning. |
| `K2LayoutCore` | Pure deterministic graph transformation: components, SCCs, ranks, ordering, coordinates, provider placement, grid policy, and statistics. | Depend on `UObject`, Slate, editor selection, pointer order, or transactions. |
| `K2RerouteRouter` | Plan spline-aware channels and safely replace eligible direct links with standard knot chains. | Reinterpret Blueprint semantics, rewrite manual knots, or mutate before a whole candidate route is accepted. |
| Generic formatter | Continue supporting non-K2 graph schemas with repaired layered behavior. | Pretend K2-only execution/data/comment rules are universally valid. |

This boundary is the main long-term defense against another engine regression. Engine integration can
change without rewriting the algorithm, and the layout core can be tested without launching Slate.

## Full formatting pipeline

### 1. Resolve graph and scope

The command chooses either the explicit selection or the full current graph. The scope is validated so
every node belongs to the target graph. Comments are tracked as containers rather than ordinary
semantic vertices. A production integration must also bind the command to the exact asset-editor
instance; global cursor/window guesses are not an acceptable automation contract.

### 2. Capture geometry without forcing Slate

The geometry adapter records node positions, node extents, and pin offsets from public graph widgets
after a normal Slate tick. It records whether each value came from live panel geometry, desired size,
persisted node state, or a deterministic pin-offset estimate. A structurally invalid snapshot aborts
before any transaction.

Persisted positions and trustworthy desired or persisted sizes can support an off-screen node. There is
deliberately no invented `160x80` (or other generic) node rectangle: if a node has no usable size, the
coordinator requests a normal panel update and retries once. A second incomplete capture atomically
aborts. Unknown node geometry is a limitation, not permission to silently claim pixel-perfect output.

### 3. Build a semantic K2 snapshot

Each semantic node receives a stable key, measured size, purity classification, and ordered ports. Each
port records:

- input or output direction;
- execution or data kind;
- measured anchor offset;
- semantic order on the node;
- whether it is a preferred execution-alignment port.

Execution and data edges are represented separately. A formatter-generated reroute chain is validated,
collapsed back to its real endpoint pair, and excluded from semantic ranking. User-authored reroutes and
malformed generated metadata remain real graph nodes.

### 4. Validate before optimizing

The adapter rejects missing endpoints, invalid directions, exec/data mismatches, inconsistent generated
chains, and unstable graph membership. This prevents a layout algorithm from normalizing malformed
topology into a different Blueprint by accident.

### 5. Find execution components and cycles

The execution graph is the control-flow skeleton. Strongly connected components condense loops into a
deterministic acyclic component graph. That lets normal flow rank left-to-right while keeping a stable
fallback for loop bodies and feedback edges.

Preservation mode uses execution-only weak components as visual event paragraphs. Pure providers attach
to their nearest downstream paragraph, while data and delegate links between impure paragraphs remain
cross-paragraph routing relationships instead of merging both layouts. Full Reflow retains the broader
all-edge component model as an explicit aggressive option.

### 6. Assign execution ranks

The condensed graph uses deterministic longest-path-style ranking. Long edges receive virtual vertices
for every intermediate rank. Virtual vertices are critical: without them, an edge that skips several
columns has no representation during crossing reduction and tends to cut through unrelated branches.

There is no `MaxLayerNodes` wrap. A rank has semantic meaning and grows vertically when necessary.

### 7. Reduce crossings under semantic constraints

Preservation ordering begins from authored Y/X positions and branch-pin order, with stable keys used only
as the final deterministic tie-break. Full Reflow begins from stable keys. Alternating median/barycenter
sweeps improve both directions, followed by bounded adjacent swaps. Exact crossing counts are measured before and after;
the accepted order must not worsen the tracked objective. Dense graphs use a deterministic work budget
rather than wall-clock timing, so a faster machine does not produce a different graph.

Branch order is a constraint, not a weak aesthetic hint. For example, `Then 0`, `Then 1`, and `Then 2`
must remain visually ordered even if swapping them would remove one crossing.

### 8. Assign coordinates and straight execution blocks

Rank widths include real node sizes, recursively measured pure-input corridors, and spacing. Each next
column starts after the widest prior node or statement compound plus at least one complete coarse layout
cell. Alignment blocks attempt to align preferred execution output and input pins, not merely node
centers. This is how straight execution wiring is achieved: node placement solves pin alignment first,
while routing remains a fallback for unavoidable detours.

In preservation mode, adjacent authored rank deltas are an expand-only lower bound. A cramped gap grows
to satisfy the widest-node/provider-corridor clearance, and that growth propagates rightward without
consuming a later generous gap. Deltas are measured independently from absolute authored positions, so
nearby event roots can share a snapped start column without changing the spacing inside either paragraph.
Interpolated authored positions on long-edge virtual ranks also contribute to those adjacent deltas, so a
skip edge keeps a readable progression through intermediate columns instead of being compressed around
the shorter real-node branch that established the ranks.

Alignment proposals are rejected when they introduce collisions or worsen protected crossings. The
result is snapped according to one of two policies:

- **Node grid** snaps conventional node positions.
- **Hybrid execution grid** snaps columns and free satellites to a configurable coarse visual cell while
  preserving execution-pin alignment even when a node top must leave the Y grid.

### 9. Place pure/data providers

Pure nodes do not determine execution ranks. They are grouped in same-column consumer-local satellites,
ordered by the corresponding input pins, and retain an authored above/below relationship when safe.
Recursively reserved corridors keep pure chains between an impure producer and consumer; an upstream
impure source forms a hard forward-X floor so the formatter cannot create the large reverse-running loops
seen in the visual regression. Shared providers use a deterministic distance-aware compromise placement.
Fallback placement searches for a clear coarse-grid slot rather than overlaying an already placed node.

This phase is what makes a Blueprint look authored rather than like a generic DAG: inputs for one node
read as a local group, while the execution spine remains easy to scan.

### 10. Pack components and avoid fixed nodes

Preservation mode retains each event paragraph's authored root anchor and top-to-bottom order. A candidate
that shifts a root by more than half a coarse cell in either axis is rejected rather than silently moving
an authored paragraph hundreds of units. Within that bound, a later paragraph may move downward when it
must clear the previous paragraph plus the configured gutter.
It never shelf-packs authored paragraphs horizontally. Full Reflow retains deterministic shelf packing.
For a partial selection, unselected nodes and unrelated comments are fixed obstacles. A planned paragraph
moves as a whole to a coarse-grid-aligned clear position; the formatter does not scatter individual nodes
around fixed content and destroy local structure.

### 11. Plan comments

Affected comments are discovered through the selected nodes and enclosing ancestors. Nested comments
are resolved inside-out. Bounds include stationary original members as well as moved members, then add
configured padding and snap outward to the grid. Plans that would capture an unrelated node or create a
new ambiguous comment overlap are rejected before mutation.

Generated presentation knots do not become semantic comment contents. The real-Blueprint corpus checks
that newly generated knots reach a first-pass fixed point in deterministic headless geometry; live Slate,
save/reload, and nested-comment fixed points remain complementary editor-level release tests.

### 12. Optionally plan wire routes

Routing is explicit because adding knots changes topology. The router receives final node positions,
real pin anchors, node obstacles, existing validated generated routes, every ordinary stationary wire in
the graph-wide context, and settings. Out-of-scope wires are reservation-only: they constrain candidate
channels without being mutated or reported as failed routes.

A direct wire remains direct when it is readable. A route is considered for backward wires, node-
obstructed wires, direct-wire conflicts, long data dependencies when enabled, or a non-trivial route
proposed by the layout core. Execution baselines have priority over data baselines; equal-priority wires
yield in stable-key order. Candidate routes use rectilinear logical channels above or below obstacles
with bounded terminal stubs and a configured knot limit.

The route search is deterministic:

1. Seed reservations for the current wire field and existing generated routes.
2. Remove the candidate wire's own baseline while evaluating it.
3. Reject candidates whose rendered spline intersects a node or itself; whose knot rectangles overlap a
   node, another knot in the candidate, or a reserved knot; or whose endpoint side would change a
   single-link manual knot's tangent direction. Preserve multi-link manual-knot endpoints when replacing
   one neighbor would make their post-install neighbor-average tangent ambiguous.
4. Collect the stable identities of every wire crossed by the current baseline. Reject any candidate
   that crosses an identity outside that set; sequential replacement therefore makes the graph-wide
   crossing-pair set monotonically non-increasing.
5. Score length, remaining wire crossings, coincident segments, near-channel crowding, knot/wire conflicts, and
   knot-box conflicts.
6. Prefer a valid layout-core route without allowing that preference to overpower safety rules.
7. Reserve the accepted rendered route and knot boxes before considering the next wire.
8. Stop deterministically when `K2RoutingPlanningWorkBudget` primitive geometry comparisons have been
   consumed, leaving the current and remaining wires unchanged.

This is a bounded best-effort router, not a global minimum-crossing solver. See [Honest limitations](#honest-limitations).

### 13. Apply once

The coordinator completes node/comment planning and builds immutable routing inputs before opening the
outer transaction. It then applies node positions and comment bounds. Within that same transaction, the
router plans one wire completely before materializing that wire, then reserves the accepted result for
the next wire. It does not pre-accept every route before the transaction begins.

The Blueprint is marked once, the graph is notified once, selection is restored, and view position/zoom
are left alone. A true no-op cancels the outer transaction and restores the package's prior dirty state.

## Topology-safe reroute materialization

The router only replaces a direct link when both endpoints still match the planned pins, their
directions are valid, the destination input is exclusive, and the reciprocal direct relationship still
exists. Source fan-out is permitted and unrelated branches must remain untouched.

For an accepted route:

1. Create standard transactional `UK2Node_Knot` objects with deterministic identities.
2. Allocate pins and stage the internal knot-to-knot chain.
3. Break only the original source/destination pair.
4. Connect the real source to the first knot and the last knot to the real destination.
5. Propagate knot pin types.
6. Tag successful knots with logical-edge and ordinal identity.
7. If either boundary connection fails, remove every staged knot, restore the exact original direct
   relationship, and verify it.

The router deliberately calls the schema's transaction-neutral base operations inside the formatter's
outer transaction. This avoids a stack of “Break Link” and “Create Node” undo entries while retaining K2
connection validation and pin notifications. If even verified restoration fails, the outer transaction
is retained so the user still has one immediate Undo escape hatch.

The formatter never silently deletes or repurposes a user-authored reroute node.

## Spline-aware routing

Logical right-angle waypoints are not the geometry Unreal draws. Stock Kismet connections are cubic
splines whose tangent depends on horizontal/vertical delta and editor spline settings. Knot nodes can
reverse the tangent direction when the average node on their output side is left of the average node on
their input side.

The router therefore converts each logical connection into the same cubic control geometry used by the
stock connection policy and adaptively flattens the curve to a polyline. Obstacle and route interaction
tests run against that rendered approximation, not only against imaginary Manhattan segments. This
matters for three common failures:

- a visually curved wire can bow under a node even when its waypoint segments do not;
- a backward knot can reverse a tangent and create a loop or self-intersection;
- two logical channels can be distinct while their rendered splines cross or nearly coincide.

The pre-mutation readability gate calls the same curve builder. It compares graph-wide node overlaps,
wire/node intersections, and execution/data wire crossings before accepting a layout. A shared pin
exempts only the exact common terminal touch: an overlapping shared segment or a later recross still
counts. The input and output pin of one user-authored knot are treated as the same physical terminal,
while distinct normal-node pins are not. User-authored knot endpoints use the same neighbor-average
tangent reversal rule as Kismet. Existing generated knots participate in collision checks at their exact
planned integer positions.
Both authored and candidate passes have deterministic primitive-work budgets and reject conservatively on
exhaustion.

Node obstacles are inflated by configured clearance. Source and destination bodies expose only their
short terminal corridors, including the opposite-side corridor used by a reverse-facing manual knot;
the rest of each endpoint node remains an obstacle. Missing Slate geometry for a knot uses its explicit
42-by-24 size and shared center pin anchor rather than a generic node fallback. Generated knot boxes are
also reservations, so a later wire should not run through the visible reroute handle.

Custom graph connection policies, custom spline settings, and future engine rendering changes can still
diverge from this model. The visual corpus must be rerun on every supported Unreal version.

## Determinism

For a fixed semantic snapshot, geometry, and settings, layout decisions must not depend on pointer
addresses, `TSet` iteration, editor frame timing, CPU speed, or a random number generator.

Implemented mechanisms include:

- stable node, pin, and edge keys derived from persistent GUIDs with deterministic graph-index/name
  fallbacks;
- explicit sorting before every order-sensitive phase;
- semantic port order as a stable constraint;
- deterministic tie-breaks for SCCs, ranks, sweeps, swaps, components, and route candidates;
- work-count budgets instead of elapsed-time cutoffs;
- deterministic generated knot GUIDs and route ordinals.

Insertion-order permutation tests exercise the engine-independent core. Determinism at the editor level
also requires a stable geometry snapshot; changing from desired or persisted sizes to newly measured
Slate sizes is new input and may legitimately change the plan.

## Idempotence

Idempotence means running the same command twice with unchanged semantic input produces no second
mutation.

The pure layout core has a repeated-build invariant. At the adapter boundary:

- validated generated knot chains collapse to their real logical edges before ranking;
- those presentation knots do not create new ranks or components;
- an existing generated chain is repositioned from its endpoint movement rather than extended;
- existing waypoint centers are preserved exactly when endpoints do not move, rather than being
  re-snapped on the second pass;
- existing routes reserve their channels on the next pass;
- a second routing pass does not append knots to a generated chain;
- an unchanged plan cancels its transaction.

This does not justify a universal claim of byte-for-byte idempotence for every editor state. Live Slate
geometry can become more accurate between passes, comment membership is engine-maintained, and custom
node widgets or connection policies can introduce new observations. Save/reload and comment/reroute
fixed-point tests belong in the release gate.

## Transaction and state-preservation contract

`Format Graph` is topology-preserving: it may move scoped nodes and resize affected comments, but it does
not create, delete, or reconnect graph nodes.

`Format + Route Wires` has a stronger guarded contract:

- geometry, node/comment plans, and immutable routing inputs are complete before the outer transaction;
- each individual wire route is fully planned before that wire's topology is mutated inside the outer
  transaction;
- every rewritten wire is validated again immediately before replacement;
- failure restores the original direct pair and removes staged knots;
- source fan-out and unrelated links are preserved;
- all accepted changes share one undo transaction;
- a no-op creates no undo history;
- the prior clean/dirty package state is preserved on a no-op or fully restored non-fatal route failure;
- selection is restored and the graph view is not intentionally moved.

The exact asset-editor/graph target is part of this contract. Toolbar commands now use the owning
editor's context objects and visible tab-manager subtree, and reject ambiguous matches. Any future
headless automation API should likewise accept an explicit graph/editor identity rather than discover a
graph from the global cursor or active top-level window.

## Validation strategy

### Current automation coverage

The engine-independent layout tests cover:

- insertion-order determinism;
- exact crossing counts and non-worsening final order;
- deterministic dense-graph work budgets;
- linear execution alignment and semantic branch order;
- scoped branch spacing and cycle condensation;
- pure-provider grouping and collision fallback;
- authored event-start clustering without collapsing full-cell or data-only columns;
- expand-only authored execution gaps, including propagation across three ranks after root clustering
  and interpolated evidence from long-edge virtual ranks;
- directional, pin-ordered provider stacks, widest-provider columns, and preservation fixed points;
- all-pairs node non-overlap;
- disconnected component packing;
- hybrid grid behavior;
- repeated-build idempotence.

The K2 routing tests cover:

- backward/obstacle routing without losing unrelated topology;
- source fan-out preservation;
- repeated-pass chain non-growth;
- maximum-knot failure preserving the direct link;
- clear forward-wire no-op and disabled data-routing no-op;
- narrow-corridor routing without rendered self-intersection;
- narrow data-route rejection when the only plan would overlap generated knot rectangles;
- deterministic generated identity and metadata;
- competing, future-baseline, and existing-route reservations;
- deterministic direct/direct crossing triggers, execution-over-data priority, and shared-terminal
  exemptions;
- shared rendered-geometry primitives, degenerate-segment symmetry, interior recross detection, and
  reservation-only stationary context wires;
- user-authored knot physical-junction exemptions, reverse-side endpoint corridors, hint-side rejection,
  and conservative multi-link tangent preservation;
- post-layout routing that cannot introduce a new crossing pair;
- bounded, insertion-order-independent planning-budget exhaustion;
- layout-core preferred waypoints and straight-hint no-op;
- malformed destination fan-in rejection;
- exact rollback after a boundary-connection failure;
- planned-pin reconstruction between planning and application, resolved through stable node/pin identity.

From the Labrador repository root, run the focused automation filter through the project task wrapper:

```powershell
python Tools\Analysis\task_cli.py test --filter "Project.Unit Tests.GraphFormatter"
```

Builds, tests, and editor validation should use the `labrador` MCP/task wrapper so an Unreal process that
exits zero with failed automation is not reported as a pass.

### Authored Blueprint and visual regression corpus

The implemented [authored Blueprint regression corpus](BLUEPRINT_CORPUS.md) formats transient copies of
11 frequently/recently edited production Blueprints. It enforces exact topology, source immutability,
bounded root/movement preservation, non-regressing readability metrics, routed logical endpoints, and
second-pass stability. Algorithm and headless asset tests still cannot completely judge “looks human,”
so the complementary screenshot/Slate-geometry corpus should cover:

- a linear event flow with differently sized nodes;
- `Branch`, `Sequence`, switch, gate, multi-gate, and loop motifs;
- nested loops and legal execution cycles;
- dense fan-out and fan-in;
- pure math/string chains feeding ordered inputs;
- one provider shared by several consumers;
- delegates, events, latent nodes, macros, collapsed graphs, and function graphs;
- user reroutes mixed with formatter reroutes;
- nested comments, selected comments, and unselected enclosing comments;
- disconnected components and partial selections around fixed nodes;
- backward, long, obstructed, and narrow-corridor wires;
- off-screen, newly spawned, custom-size, and fallback-geometry nodes;
- undo, redo, save, reload, compile, and a second format pass.

Capture both graph data and screenshots. A `.T3D`/topology hash can prove links were preserved; a
screenshot or geometry manifest proves the visual result.

### Metrics and acceptance gates

| Invariant or metric | Release expectation |
|---|---|
| Semantic topology hash | Identical before/after `Format Graph`; after routing, identical after collapsing validated generated knots. |
| Invalid pin links / Blueprint compile | Zero. |
| Semantic node overlaps | Zero. |
| Fixed-node/comment safety violations | Zero. |
| Branch-pin order violations | Zero. |
| Execution crossings | Must not increase in the core ordering phase; track absolute result on the visual corpus. |
| Straight primary execution links | Track ratio and prevent corpus regressions. |
| Backward execution/data links | Never introduce backward execution; reject material backward-data regressions. |
| Authored horizontal spacing | Expand gaps required by configured gutters, propagate that expansion, and never consume a later generous adjacent-rank gap. |
| Authored event roots | Limit drift in both axes to coarse-grid snap tolerance and never invert top-to-bottom order. |
| Unrouted wire/node intersections | Reject newly introduced rendered-spline intersections before mutation; evaluate direct and generated routes with the same flattened Kismet curve model. |
| Rendered wire crossings | The pre-mutation layout gate permits no new execution or data crossing-pair identity, including equal-count substitutions; every installed reroute is additionally crossing-pair monotonic against the complete reserved wire field. |
| Wire/node intersections | Zero for accepted generated routes under the stock Kismet policy. |
| Bends and generated knots | Minimize after safety/crossings; enforce the per-wire cap and reject node/knot-box overlap. |
| Determinism | Identical plan across snapshot insertion permutations. |
| Idempotence | Second pass produces zero node/comment/knot changes once geometry is stable. |
| Undo/redo | One operation restores/reapplies exact positions, bounds, and topology. |
| Performance | Record node/edge counts and phase work; use deterministic budgets, never time-dependent output. |

Property/fuzz tests should generate small typed K2-like graphs and assert totality, no overlap, stable
output, non-worsening crossings, and collapse-equivalent topology. Failing seeds must be reproducible.

## Honest limitations

The current architecture intentionally does not promise the following:

- **No global optimum.** Crossing minimization is heuristic, as it is in practical layered layout
  engines. The router searches bounded channel families and processes wires deterministically; it does
  not solve a global multi-commodity routing problem.
- **Crossings can still remain.** Direct-wire intersections now trigger deterministic routing attempts,
  but a wire remains unchanged if every bounded candidate is unsafe, the knot cap is too small, or the
  planning-work budget is exhausted. There is no global rip-up/re-route optimizer yet.
- **The pre-mutation gate is deliberately conservative.** A newly introduced rendered direct curve through a
  node rejects the layout even when a later routing search might have found a detour. Existing generated
  routes are assessed from their exact planned waypoint curves, but new-route planning still occurs
  transactionally after the layout has passed this gate.
- **Rendered-wire crowding is heuristic.** Adaptive spline flattening and a general angle/distance cost
  cover intersections and near-parallel microsegments, but they do not prove globally uniform clearance
  across every wire in a dense graph.
- **No automatic bus/junction synthesis.** The router creates standard one-wire knot chains. It does not
  merge several logical wires into a shared semantic bus, because an incorrect junction would change
  meaning.
- **No deletion of manual reroutes.** User-authored knots remain semantic nodes even when the formatter
  could draw a shorter wire without them.
- **Pin geometry can be approximate; node geometry cannot be invented.** Missing live pin anchors can
  use deterministic ordered offsets. An unticked/off-screen/custom node may use a trustworthy desired or
  persisted size, but if no usable node size exists after one normal-update retry, the entire operation
  aborts without mutation.
- **Stock Kismet rendering is the model.** A custom connection drawing policy can render a different
  curve from the router's prediction.
- **Large dense routing is not free.** Candidate interactions with reserved splines trend toward
  quadratic work. Candidate counts are bounded, but very large graphs need profiling and possibly a
  spatial index.
- **The generic path is not K2 god mode.** Materials, Niagara, PCG, Behavior Trees, and other graph
  schemas retain repaired generic layout and do not receive K2 execution/data/reroute semantics.
- **Editor integration is part of correctness.** Multiple asset-editor windows, unusual toolkits, and
  automation require exact target binding; layout-core correctness cannot compensate for formatting
  the wrong visible graph.
- **Licenses are not performance features.** ELK, libavoid, OGDF, Graphviz, MSAGL, or dagre should not be
  vendored casually because a screenshot looked good. Any backend must pass the same topology,
  determinism, integration, and distribution gates.

## Roadmap to “god mode”

### Phase 0: complete integration validation

- Add an automated multi-editor regression for the implemented exact, ambiguity-rejecting toolbar target
  binding; preserve that contract in any future automation surface.
- Extend the implemented GUID-authenticated/canonically parsed route metadata coverage through save and
  reload.
- Add full editor-level comment+routing idempotence coverage for the implemented presentation-knot
  exclusion.
- Consider conservative class-specific size caches only if retry-then-abort proves too restrictive; any
  cache must be versioned and must never silently override trustworthy measured or persisted geometry.
- Grow malformed-metadata and transaction/undo integration tests beyond the current router unit fixtures.

### Phase 1: global channel router

- Build a sparse visibility/channel graph from inflated node rectangles, port escape corridors, comment
  boundaries, and reserved knot boxes.
- Use deterministic A* or multi-source shortest path with a lexicographic cost: safety, crossings,
  overlaps, bends, length, movement from the prior route, then stable key.
- Add rip-up and re-route improvement passes so early wires do not detour around baselines that later
  disappear.
- Use a spatial index and general spline-segment distance for scalable wire clearance.

### Phase 2: Blueprint motif intelligence

- Recognize branch diamonds, sequences, switch fans, loopbacks, gates, event blocks, and pure-provider
  trees.
- Give each motif a readable lane template while retaining the global rank/crossing constraints.
- Support explicit, topology-safe reroute junction strategies for visual fan-out grouping.
- Add optional wire bundling only when it cannot imply a false shared value or execution path.

### Phase 3: incremental and interactive stability

- Add user locks for nodes, columns, comments, and routes.
- Reformat the affected component after a local edit instead of repacking the whole graph.
- Present a preview with quality metrics and allow layout-only versus layout+route acceptance.
- Preserve prior channel identities where they remain safe.

Preservation-first positioning, authored event-paragraph order, coarse-cell statement columns, and the
pre-mutation readability rejection gate are implemented. This phase now concerns explicit user locks,
incremental scope selection, and interactive preview rather than basic mental-map preservation.

### Phase 4: evidence-driven tuning

- Grow the real Blueprint corpus and store per-graph metrics.
- Compare native results offline against ELK/Graphviz and, if legally evaluated, libavoid; use them as
  oracles, not shipping requirements.
- Run pairwise human reviews focused on scan time and error rate, not only compactness.
- Tune presets such as `Readable`, `Compact`, `Minimal Movement`, and `Aggressive Routing` without
  weakening topology invariants.

## Definition of ultimate

The formatter earns “ultimate” when it can make and sustain these claims on a representative corpus:

- topology is provably unchanged except for collapsible, owned presentation reroutes;
- every semantic node is non-overlapping and grid-consistent;
- execution flow is immediately readable and branch order matches pins;
- inputs are grouped locally around their consumers;
- generated wires avoid nodes under the actual stock spline policy;
- crossings and bends are measurably reduced without needless graph growth;
- a repeated pass is a no-op after geometry stabilizes;
- output is identical across insertion permutations and machines;
- one Undo restores the exact prior graph;
- partial selection, comments, custom nodes, multiple editors, save/reload, and malformed inputs fail
  safely;
- limitations and skipped routes are surfaced rather than hidden behind a success toast.

That is a higher bar than “the nodes moved.” It is also the bar required for a graph formatter that
developers can trust on production Blueprints.

## Primary references

- [Original MIT Graph Formatter](https://github.com/howaajin/graphformatter)
- [ELK Layered algorithm](https://eclipse.dev/elk/reference/algorithms/org-eclipse-elk-layered.html)
- [ELK port constraints](https://eclipse.dev/elk/reference/options/org-eclipse-elk-portConstraints.html)
- [ELK model-order and interactive constraints](https://eclipse.dev/elk/blog/posts/2023/23-01-09-constraining-the-model.html)
- [Adaptagrams libavoid](https://www.adaptagrams.org/documentation/libavoid.html)
- [Graphviz dot](https://graphviz.org/docs/layouts/dot/)
- [Fast and Simple Horizontal Coordinate Assignment](https://link.springer.com/chapter/10.1007/3-540-45848-4_3)
- [Size- and Port-Aware Horizontal Node Coordinate Assignment](https://link.springer.com/chapter/10.1007/978-3-319-27261-0_12)
