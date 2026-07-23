# Graph Formatter for Unreal Engine

Graph Formatter arranges Unreal Engine graphs automatically. Blueprint/K2 graphs use a deterministic, execution-first formatter designed for human-readable flow; other supported graph editors use the repaired generic layered formatter.

The plugin remains based on Howaajin's MIT-licensed [Graph Formatter](https://github.com/howaajin/graphformatter).

For the regression diagnosis, dependency/license evaluation, native architecture, routing model, validation plan, limitations, and roadmap, read [Ultimate Blueprint Graph Formatter](Docs/ULTIMATE_FORMATTER.md).

## Usage

Select exactly the nodes you want to arrange, or deselect everything to format the whole graph.

- **Format Graph** moves nodes and resizes selected comments plus any enclosing ancestor comments without changing graph topology. It is intentionally unbound by default so it does not replace stock Blueprint **Find** (`Ctrl+F`); assign a chord under **Editor Preferences > Keyboard Shortcuts** if desired.
- **Format + Route Wires** performs the same layout and may add standard `UK2Node_Knot` reroute nodes to crossing, obstructed, backward, or unusually long wires. Routing is intentionally opt-in because it changes graph topology.
- **Compare A/B/C/D** opens a blinded five-pane bakeoff: the unchanged graph is the reference, while randomized candidates A/B/C/D are this formatter, Blueprint Auto Layout, this formatter with Adaptagrams/libavoid routing, and elkjs/ELK Layered with orthogonal routing. Every candidate is a read-only transient Blueprint copy; the source graph is never changed.
- **Place Block** (`Ctrl+E`) keeps the existing block-placement workflow.
- **Flat Spline Rendering** only toggles Unreal's global zero-tangent wire rendering. It is not a layout or routing command.

Configure the formatter under **Editor Preferences > Plugins > Graph Formatter**. The settings use Unreal's `EditorPerProjectUserSettings` config domain and are saved per user for this project. The values below are constructor defaults, not shared project overrides.

## Blueprint/K2 layout

The K2 formatter separates execution flow from data dependencies instead of treating every pin equally:

- **Preserve Human Layout** is the default: authored event islands retain their vertical reading order and local mental map. Their starts share the scope-wide median major-grid X column, while vertical movement is limited to snapping and the extra separation needed to keep successive islands clear. Broad redraws are rejected unless a concrete readability gain justifies a bounded local edit. **Full Reflow** remains available as an explicit aggressive rebuild.
- Event-rooted execution islands are discovered from execution wiring only. Delegate and data links may cross between islands without flattening them into one layout paragraph.
- Execution strongly determines left-to-right ranks.
- The primary execution path is pin-aligned so wires stay visually straight where integer node positions permit it.
- Execution-pin alignment has priority over node-top Y snapping. Columns, event anchors, and provider satellites use the visible major Blueprint canvas grid (128 graph units with Unreal's default 16-unit snap and 8-cell rule period).
- Every execution column begins after the widest node or pure-input compound in the previous column, with at least one complete layout cell of empty horizontal space. Preserve mode expands cramped authored gaps but propagates that expansion forward instead of compacting any later generous rank-to-rank gap.
- Branch output order is a hard ordering constraint; alternating median/barycenter sweeps and adjacent swaps reduce crossings around it.
- Long execution edges use virtual layout vertices, avoiding rank compression and arbitrary layer wrapping.
- Cycles are condensed deterministically and laid out with a stable fallback.
- Pure data providers are placed in consumer-local, pin-ordered columns. Their authored above/below side is stable, collision stacks grow outward from the consumer, and the widest provider defines the shared column. Impure upstream sources constrain pure chains to remain forward-running.
- Event islands are stacked in authored top-to-bottom order with generous configurable gutters; preserve mode never shelf-packs them horizontally. Every execution root in the formatted scope shares one coarse-grid X anchor chosen from the authored median. Data-only islands are not coerced into that column.
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
- Before mutation, the adapter compares authored and proposed readability across all graph nodes and wires. It uses the same flattened Kismet spline geometry as routing, including reverse-facing user reroute tangents, and rejects candidates that introduce overlaps (including moved generated knots), backward execution, a bend in a preferred straight execution link (including links crossing a partial-selection boundary), materially worse backward data wiring, new wire-under-node paths, or a higher total execution/data crossing multiplicity. Equal-count crossing substitutions are permitted during placement so one changed pair cannot veto an otherwise better graph; installed reroutes retain the stricter per-wire crossing-identity monotonic gate. Preserve mode moves event starts toward one shared major-grid column, preserves authored event order, and rejects broad node movement that is disproportionate to the measured gain. Existing generated routes are compared using their exact planned waypoint curves; intentional downstream X expansion is not misclassified as damage. The check is deterministically budgeted and fails closed.
- The Blueprint is marked modified and the graph is notified once after a successful format.

## Other graph editors

Behavior Trees, Materials, Niagara, PCG, Sound Cues, and other enabled graph types retain the generic layered formatter. Its deterministic ordering, crossing baseline, conflict detection, median calculation, disconnected orientation, and parameter grouping have been repaired. The K2-only grid, pure-provider, and reroute semantics are not imposed on graph schemas that do not support them.

## Blinded formatter bakeoff

**Compare A/B/C/D** captures the active graph's rendered node sizes and pin offsets once, remaps that geometry to read-only transient Blueprint duplicates, and runs all candidates against the same starting topology and current spacing settings. It refuses fallback-only snapshots (for example, zero Slate-sized nodes or zero pin anchors on a wired graph) and automatically retries after one normal panel tick instead of recording misleading 100% coverage. Once the five read-only panes render, their actual Slate geometry is recaptured and every metric is recomputed. The duplicates use Unreal's own Blueprint-merge compilation-suppression guard, so self-typed pins retain a valid Blueprint context without compiling the copies. Candidate order is randomized and backend names plus objective metrics stay hidden until **Reveal formatter mapping** is pressed.

The ballot records five independent judgments: overall readability, execution flow, input grouping, wire routing, and preservation of authored intent. Candidate buttons toggle independently, so each criterion can record one winner or any two-, three-, or four-candidate tie; **All tied** is a shortcut for selecting all four. Revealing identities permanently locks blind voting for that run. A run manifest and optional ballot are written beneath `Saved/GraphFormatter/Bakeoff/<run-id>/`; after the panes stabilize, the comparison automatically captures `original.png` plus blinded `candidate_A.png`, `candidate_B.png`, `candidate_C.png`, and `candidate_D.png`, and saving/revealing retries capture if needed. All five panes use the same zoom factor, centered independently, so a sprawling result cannot look deceptively compact merely because `Zoom to Fit` scaled it farther down. Ballot schema v2 stores the visible labels and stable resolved backend IDs as deterministically ordered arrays for later aggregation. Manifest schema v4 and readability model v3 record runtime, topology and readability-safety validity, backend configuration and adapter telemetry, source and rendered node/pin geometry coverage, screenshot dimensions, diagnostics, and raw penalties for overlaps, backward edges, execution bends, wire crossings, wire/node intersection pairs, sub-cell execution gaps, grid alignment, reroutes, wire length, drawing area, semantic movement, and reroute movement. Added reroutes are penalized relative to the original graph, and candidates that introduce overlaps, wire-under-node hits, backward execution, execution crossings, or sub-cell execution gaps are visibly marked **INVALID**. The composite penalty is a regression signal, not a replacement for the blinded human decision.

The libavoid comparison is deliberately conservative: GraphFormatter first identifies direct wires with an actual backward, obstruction, or crossing defect; libavoid proposes orthogonal routes with the complete direct-wire field present; and the production rendered-spline safety gate validates that exact proposal. An unsafe libavoid route is rejected instead of silently being replaced by a native route under the libavoid label. Clear wires remain direct. Blueprint Auto Layout receives the same measured node sizes and captured pin-Y offsets as the other candidates, with its upstream deterministic pin-order estimate only for uncaptured pins; its vertical, branch, root, and pure-node padding are clamped to at least one major layout cell. These adapter decisions and their accepted/rejected counts are included in `run.json` so a poor result can be distinguished from incomplete input or an aggressive integration policy.

The ELK candidate is an independent general-purpose oracle rather than another wrapper around native placement. Its input records every non-comment node at its measured size and every linked pin as a fixed-position west/east port. Execution roots and sinks receive first/last-layer constraints; execution edges receive much stronger direction and straightness priority than data edges; original vertical order is supplied as model order; and spacing is rounded up with an extra major cell before ELK Layered performs crossing minimization, Brandes-Koepf placement, and orthogonal routing. Returned positions are re-anchored near the authored graph, snapped to the configured K2 layout cell, and returned edge bends are materialized as ordinary reroute nodes on the transient candidate only. Each UI run preserves `elk_input.json`, `elk_output.json`, and `elk_process.txt` beside `run.json`, making the exact submitted data, raw solver result, executable, exit code, and stderr auditable.

The vendored elkjs bundle is not loaded by the production formatter. The comparison launches it through a local `node` executable; set `GRAPHFORMATTER_ELK_NODE` to an absolute Node.js executable path when `node` is not on `PATH`. If the runtime or bundle is unavailable, only the ELK pane is marked invalid and its diagnostic is recorded—the source graph remains untouched.

The compared implementations are pinned and deliberately scoped:

- Blueprint Auto Layout 0.6.9 (`de8394f2f83cfc02e4595c6c15e2eb655fae1c55`) contributes only its MIT-licensed callable layered layout and routing implementation. Its toolbar, settings, and startup module are excluded.
- Adaptagrams/libavoid (`840ebcff20dbba36ad03a2160edf7cbaf9859984`) is LGPL-2.1-or-later and lives in the separately built, dynamically loaded `GraphFormatterAdaptagrams` editor DLL. No Adaptagrams type crosses that module boundary, and the DLL loads only when a comparison actually requests it. It is currently a routing candidate paired with GraphFormatter placement—not a full HOLA placement candidate.
- elkjs 0.12.0 (`ff5771d7165445c42c408bb8a090c8035272218c`) is used under EPL-2.0 as an unmodified, benchmark-only ELK Layered bundle launched out of process. The MIT adapter and pinned upstream metadata live beneath `Resources/ElkJs`; no JavaScript or ELK code is linked into the production formatting path.

The checked-in backend test runs all four implementations on a transient copy of `BPC_ResourceCarrier.DropHeldActor`, verifies logical topology and source immutability, checks that ELK receives ports/edges and returns routes that can be materialized, and separately checks libavoid obstacle avoidance and deterministic output. A dedicated audit-artifact test parses the generated ELK input, output, and process records and verifies solver options, fixed measured ports, root/sink constraints, edge priorities, successful execution, and source immutability. ELK execution assertions are skipped with a warning only when no external Node.js runtime can be launched.

## Installing

Clone or download this fork into `[ProjectRoot]/Plugins/GraphFormatter`, then restart the editor. A source build requires the normal Unreal Engine C++ toolchain. The [original project's releases](https://github.com/howaajin/graphformatter/releases) and [Marketplace page](https://www.unrealengine.com/marketplace/graph-formatter) provide the upstream legacy plugin; they do not necessarily include this fork's semantic K2 formatter or router.

## Technical background

The generic formatter and new K2 core draw on layered graph drawing and port-aware coordinate assignment:

- [Layered graph drawing](https://en.wikipedia.org/wiki/Layered_graph_drawing)
- [Fast and Simple Horizontal Coordinate Assignment](https://link.springer.com/chapter/10.1007/3-540-45848-4_3)
- [Size- and Port-Aware Horizontal Node Coordinate Assignment](https://link.springer.com/chapter/10.1007/978-3-319-27261-0_12)

The K2 core additionally uses deterministic strongly connected component condensation, longest-path ranking, virtual long-edge vertices, constrained crossing reduction, alignment blocks, and component packing. It is engine-state independent and covered by automation tests. A separate [authored Blueprint regression corpus](Docs/BLUEPRINT_CORPUS.md) formats transient copies of frequently/recently edited production graphs and enforces topology, readability, movement, routing, source-immutability, and second-pass stability gates.

The reason the production formatter owns a native core instead of depending on ELK, libavoid, Graphviz, OGDF, MSAGL, or dagre is documented in the [architecture and ecosystem decision report](Docs/ULTIMATE_FORMATTER.md#ecosystem-and-license-decision-matrix). Adaptagrams/libavoid and elkjs are present only as optional bakeoff oracles so their results can be judged on real graphs without making either a production-layout dependency. The report also lists current limitations and the remaining work required for globally optimized, incremental “god mode” routing.

For upstream documentation and support, see the [Graph Formatter wiki](https://github.com/howaajin/graphformatter/wiki) and [issue tracker](https://github.com/howaajin/graphformatter/issues).
