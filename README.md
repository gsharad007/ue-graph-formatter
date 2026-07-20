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

- Execution strongly determines left-to-right ranks.
- The primary execution path is pin-aligned so wires stay visually straight where integer node positions permit it.
- Branch output order is a hard ordering constraint; alternating median/barycenter sweeps and adjacent swaps reduce crossings around it.
- Long execution edges use virtual layout vertices, avoiding rank compression and arbitrary layer wrapping.
- Cycles are condensed deterministically and laid out with a stable fallback.
- Pure data providers are placed upstream and grouped near the consumer inputs they feed.
- Disconnected components are packed with generous spacing.
- Hybrid grid snapping snaps columns and alignment blocks while preserving execution-pin alignment.
- Fixed, unselected nodes are obstacles. Formatted components move clear of them rather than overlapping them.
- Selected comments include their contents recursively. Nested comments and unselected ancestors affected by moved nodes are resized inside-out without dropping stationary contents.

The formatter captures public Slate geometry after a normal editor tick. Persisted positions and trustworthy desired or persisted sizes can support off-screen nodes, while pin anchors can use deterministic ordered offsets when live pin geometry is unavailable. If any node still has no measurable or persisted size, the formatter requests one normal panel update, retries once, and then aborts before mutation. It never invents a generic node rectangle, alters zoom, ticks widgets manually, or accesses private Slate members.

## Wire routing

`Format + Route Wires` uses deterministic rectilinear logical channels and only materializes a route after every waypoint is known to be safe under an approximation of Unreal's rendered Kismet spline. Accepted routes reserve their rendered curves and knot boxes, so later routed wires avoid coincident segments and prefer lower-crossing alternatives. Long execution edges can reuse the layout core's virtual-rank waypoints. Each wire is replaced transactionally; if Unreal's K2 schema rejects any part of a knot chain, the chain is removed and the original direct connection is restored and verified.

Generated knots are tagged as Graph Formatter routes. Later layout passes validate and collapse those chains back to their real logical endpoints and exclude the generated presentation nodes from ranking. The engine-independent core has repeated-build coverage, and repeated routing does not extend a validated generated chain. Knot creation and pin rewiring bypass Unreal's nested user-action transactions and per-link Blueprint dirtying; the entire operation is one undo step and the Blueprint is marked once. Direct-wire crossings are routing triggers: execution wires keep priority over data wires, and equal-priority wires yield in stable order. Routing remains best effort—a wire stays unchanged when no safe candidate fits the knot cap or when the deterministic planning budget is exhausted. This is not a global minimum-crossing solver.

## K2 settings

| Setting | Purpose |
|---|---|
| `bEnableK2Formatter` | Enables the semantic K2 path. Unsupported graphs automatically use the generic formatter. |
| `bEnableHybridGridSnap` | Uses alignment-preserving grid snapping; when disabled, nodes use conventional grid snapping. |
| `bPreserveComponentAnchor` | Keeps the formatted scope near its authored top-left while snapping that anchor to the graph grid. |
| `K2OrderingSweeps` | Controls deterministic crossing-reduction effort. |
| `K2RoutingPlanningWorkBudget` | Caps primitive routing-geometry comparisons per operation; the constructor default is 1,000,000. |
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

The constructor defaults favor readability over compactness: 160 units between execution columns, 96 units of node and branch clearance, 256 units between components, and 64 units of comment padding.

## Safety and determinism

- Standard formatting never creates or removes nodes or connections.
- Routing is a separate explicit command.
- Node/comment changes and routing inputs are calculated before opening the outer transaction. The router then fully plans each wire before mutating that wire under the same transaction.
- A no-op cancels the outer transaction, creates no undo entry, and restores the package's prior dirty state.
- Node moves, comment bounds, and generated knots share one transaction.
- Selection and graph view are preserved.
- Stable node, pin, and edge identifiers—not pointer or container order—break layout ties.
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

The K2 core additionally uses deterministic strongly connected component condensation, longest-path ranking, virtual long-edge vertices, constrained crossing reduction, alignment blocks, and component packing. It is engine-state independent and covered by automation tests.

The reason this fork owns a native core instead of embedding ELK, libavoid, Graphviz, OGDF, MSAGL, or dagre is documented in the [architecture and ecosystem decision report](Docs/ULTIMATE_FORMATTER.md#ecosystem-and-license-decision-matrix). The report also lists current limitations and the remaining work required for globally optimized, incremental “god mode” routing.

For upstream documentation and support, see the [Graph Formatter wiki](https://github.com/howaajin/graphformatter/wiki) and [issue tracker](https://github.com/howaajin/graphformatter/issues).
