# Graph Formatter for Unreal Engine

Graph Formatter arranges Unreal Engine graphs automatically. Blueprint/K2 graphs use a deterministic, execution-first formatter designed for human-readable flow; other supported graph editors use the repaired generic layered formatter.

The plugin remains based on Howaajin's MIT-licensed [Graph Formatter](https://github.com/howaajin/graphformatter).

For the regression diagnosis, dependency/license evaluation, native architecture, routing model, validation plan, limitations, and roadmap, read [Ultimate Blueprint Graph Formatter](Docs/ULTIMATE_FORMATTER.md).

## Usage

Select exactly the nodes you want to arrange, or deselect everything to format the whole graph.

- **Format Graph** moves nodes and resizes selected comments plus any enclosing ancestor comments without changing graph topology. It is intentionally unbound by default so it does not replace stock Blueprint **Find** (`Ctrl+F`); assign a chord under **Editor Preferences > Keyboard Shortcuts** if desired.
- **Format + Route Wires** performs the same layout and may add standard `UK2Node_Knot` reroute nodes to crossing, obstructed, backward, or unusually long wires. Routing is intentionally opt-in because it changes graph topology.
- **Place Block** (`Ctrl+E`) keeps the existing block-placement workflow.
- **Flat Spline Rendering** only toggles Unreal's global zero-tangent wire rendering. It is not a layout or routing command.

Configure the formatter under **Editor Preferences > Plugins > Graph Formatter**. The settings use Unreal's `EditorPerProjectUserSettings` config domain and are saved per user for this project. The values below are constructor defaults, not shared project overrides.

## Blueprint/K2 layout

The K2 formatter separates execution flow from data dependencies instead of treating every pin equally:

- **Preserve Human Layout** is the default: authored event islands retain their start position (within half a coarse snap cell), vertical reading order, and local mental map. Broad redraws are rejected unless a concrete readability gain justifies a bounded local edit. **Full Reflow** remains available as an explicit aggressive rebuild.
- Event-rooted execution islands are discovered from execution wiring only. Delegate and data links may cross between islands without flattening them into one layout paragraph.
- Execution strongly determines left-to-right ranks.
- The primary execution path is pin-aligned so wires stay visually straight where integer node positions permit it.
- Execution-pin alignment has priority over node-top Y snapping. Columns, event anchors, and provider satellites use the visible major Blueprint canvas grid (128 graph units with Unreal's default 16-unit snap and 8-cell rule period).
- Every execution column begins after the widest node or pure-input compound in the previous column, with at least one complete layout cell of empty horizontal space. Preserve mode expands cramped authored gaps but propagates that expansion forward instead of compacting any later generous rank-to-rank gap.
- Branch output order is a hard ordering constraint; alternating median/barycenter sweeps and adjacent swaps reduce crossings around it.
- Long execution edges use virtual layout vertices, avoiding rank compression and arbitrary layer wrapping.
- Cycles are condensed deterministically and laid out with a stable fallback.
- Pure data providers are placed in consumer-local, pin-ordered columns. Their authored above/below side is stable, collision stacks grow outward from the consumer, and the widest provider defines the shared column. Impure upstream sources constrain pure chains to remain forward-running.
- Event islands are stacked in authored top-to-bottom order with generous configurable gutters; preserve mode never shelf-packs them horizontally. Nearby event roots authored as one start column share one coarse-grid X anchor, while roots a full cell apart remain distinct.
- Hybrid grid snapping snaps columns and alignment blocks while preserving execution-pin alignment.
- Fixed, unselected nodes are obstacles. Formatted components move clear of them rather than overlapping them.
- Selected comments include their contents recursively. Nested comments and unselected ancestors affected by moved nodes are resized inside-out without dropping stationary contents.

The formatter captures public Slate geometry after a normal editor tick. Persisted positions and trustworthy desired or persisted sizes can support off-screen nodes, while pin anchors can use deterministic ordered offsets when live pin geometry is unavailable. If any node still has no measurable or persisted size, the formatter requests one normal panel update, retries once, and then aborts before mutation. It never invents a generic node rectangle, alters zoom, ticks widgets manually, or accesses private Slate members.

## Wire routing

`Format + Route Wires` uses deterministic rectilinear logical channels and only materializes a route after every waypoint is known to be safe under an approximation of Unreal's rendered Kismet spline. The complete graph-wide wire field—including stationary wires outside a partial selection—reserves its rendered curves, and accepted routes additionally reserve their knot boxes. Candidate knot rectangles may not overlap semantic nodes, other knots on the same route, or reserved generated knots. A candidate may intersect only wire identities that its current baseline already intersects, so sequential rerouting can preserve or remove crossing pairs but cannot introduce a new pair after the layout safety gate. Long execution edges can reuse the layout core's virtual-rank waypoints. Each wire is replaced transactionally; if Unreal's K2 schema rejects any part of a knot chain, the chain is removed and the original direct connection is restored and verified.

Generated knots are tagged as Graph Formatter routes. Later layout passes validate and collapse those chains back to their real logical endpoints and exclude the generated presentation nodes from ranking. The engine-independent core has repeated-build coverage, and repeated routing does not extend a validated generated chain. Knot creation and pin rewiring bypass Unreal's nested user-action transactions and per-link Blueprint dirtying; the entire operation is one undo step and the Blueprint is marked once. Direct-wire crossings are routing triggers: execution wires keep priority over data wires, and equal-priority wires yield in stable order. Reverse-facing manual-knot endpoints retain their rendered side; multi-link manual-knot junctions stay authored when replacing one neighbor could change Unreal's average-based tangent direction. Routing remains best effort—a wire stays unchanged when no safe candidate fits the knot cap or when the deterministic planning budget is exhausted. This is not a global minimum-crossing solver.

## K2 settings

| Setting | Purpose |
|---|---|
| `bEnableK2Formatter` | Enables the semantic K2 path. Unsupported graphs automatically use the generic formatter. |
| `bEnableHybridGridSnap` | Uses alignment-preserving grid snapping; when disabled, nodes use conventional grid snapping. |
| `K2LayoutMode` | Selects default **Preserve Human Layout** or explicit **Full Reflow**. The legacy whole-scope `bPreserveComponentAnchor` value is retained only for config compatibility. |
| `K2LayoutCellSize` | Minimum coarse visual cell for event anchors, statement columns, provider rows, and gutters. It is rounded up to a whole visible major Blueprint canvas square and defaults to 128 units. |
| `K2OrderingSweeps` | Controls deterministic crossing-reduction effort. |
| `K2RoutingPlanningWorkBudget` | Caps primitive geometry work independently for each graph-wide readability pass and for routing; the constructor default is 1,000,000. Exhaustion conservatively leaves the layout or remaining wires unchanged. |
| `K2HorizontalSpacing`, `K2VerticalSpacing`, `K2BranchSpacing` | Control execution-column and branch-lane clearance. |
| `K2ComponentSpacing` | Controls separation between disconnected components. |
| `K2PureHorizontalSpacing`, `K2PureVerticalSpacing` | Control pure-provider grouping. |
| `K2CommentPadding` | Controls resized comment padding. |
| `K2ObstacleClearance` | Controls node avoidance during layout and routing. |
| `K2RoutingChannelSpacing` | Controls separation of routing channels. |
| `K2MaxGeneratedKnots` | Caps generated knots per logical wire. |
| `K2LongDataWireRankThreshold` | Selects long data wires for opt-in routing. |
| `bRouteDataWires` | Allows crossing, backward, long, or obstructed data wires to be routed. |
| `bShowLayoutNotifications` | Shows success and safe-abort notifications. |

The constructor defaults favor readability over compactness: preservation mode, a 128-unit major-grid cell, 160 units between execution columns, 96 units of node and branch clearance, 256 units between event paragraphs, and 64 units of comment padding. Spacing values smaller than one layout cell are promoted to one complete cell.

## Safety and determinism

- Standard formatting never creates or removes nodes or connections.
- Routing is a separate explicit command.
- Node/comment changes and routing inputs are calculated before opening the outer transaction. The router then fully plans each wire before mutating that wire under the same transaction.
- A no-op cancels the outer transaction, creates no undo entry, and restores the package's prior dirty state.
- Node moves, comment bounds, and generated knots share one transaction.
- Selection and graph view are preserved.
- Stable node, pin, and edge identifiers—not pointer or container order—break layout ties.
- Before mutation, the adapter compares authored and proposed readability across all graph nodes and wires. It uses the same flattened Kismet spline geometry as routing, including reverse-facing user reroute tangents, and rejects candidates that introduce overlaps (including moved generated knots), backward execution, a bend in a preferred straight execution link (including links crossing a partial-selection boundary), materially worse backward data wiring, new wire-under-node paths, or any new execution/data crossing-pair identity—even when another crossing disappeared and the total count stayed equal. Preserve mode limits event-start drift in both axes, preserves authored event order, and rejects broad node movement that is disproportionate to the measured gain. Existing generated routes are compared using their exact planned waypoint curves; intentional downstream X expansion is not misclassified as damage. The check is deterministically budgeted and fails closed.
- The Blueprint is marked modified and the graph is notified once after a successful format.

## Other graph editors

Behavior Trees, Materials, Niagara, PCG, Sound Cues, and other enabled graph types retain the generic layered formatter. Its deterministic ordering, crossing baseline, conflict detection, median calculation, disconnected orientation, and parameter grouping have been repaired. The K2-only grid, pure-provider, and reroute semantics are not imposed on graph schemas that do not support them.

## Installing

Clone or download this fork into `[ProjectRoot]/Plugins/GraphFormatter`, then restart the editor. A source build requires the normal Unreal Engine C++ toolchain. The [original project's releases](https://github.com/howaajin/graphformatter/releases) and [Marketplace page](https://www.unrealengine.com/marketplace/graph-formatter) provide the upstream legacy plugin; they do not necessarily include this fork's semantic K2 formatter or router.

## Technical background

The generic formatter and new K2 core draw on layered graph drawing and port-aware coordinate assignment:

- [Layered graph drawing](https://en.wikipedia.org/wiki/Layered_graph_drawing)
- [Fast and Simple Horizontal Coordinate Assignment](https://link.springer.com/chapter/10.1007/3-540-45848-4_3)
- [Size- and Port-Aware Horizontal Node Coordinate Assignment](https://link.springer.com/chapter/10.1007/978-3-319-27261-0_12)

The K2 core additionally uses deterministic strongly connected component condensation, longest-path ranking, virtual long-edge vertices, constrained crossing reduction, alignment blocks, and component packing. It is engine-state independent and covered by automation tests. A separate [authored Blueprint regression corpus](Docs/BLUEPRINT_CORPUS.md) formats transient copies of frequently/recently edited production graphs and enforces topology, readability, movement, routing, source-immutability, and second-pass stability gates.

The reason this fork owns a native core instead of embedding ELK, libavoid, Graphviz, OGDF, MSAGL, or dagre is documented in the [architecture and ecosystem decision report](Docs/ULTIMATE_FORMATTER.md#ecosystem-and-license-decision-matrix). The report also lists current limitations and the remaining work required for globally optimized, incremental “god mode” routing.

For upstream documentation and support, see the [Graph Formatter wiki](https://github.com/howaajin/graphformatter/wiki) and [issue tracker](https://github.com/howaajin/graphformatter/issues).
