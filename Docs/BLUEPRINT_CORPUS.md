# Authored Blueprint regression corpus

The Graph Formatter integration corpus protects real, hand-arranged project graphs instead of relying only on synthetic graph motifs. It duplicates each complete Blueprint under `/Engine/Transient` before formatting, preserving Unreal's event-node ownership invariants while ensuring an automation run cannot move nodes, add reroutes, dirty, save, or otherwise mutate a source asset.

The focused test filter is:

```text
Project.Unit Tests.GraphFormatter.K2Corpus.PreserveAuthoredBlueprints
```

Use the Labrador `test` task with that filter. Build and test through Labrador so Unreal automation failures are read from the generated report rather than inferred from the editor process exit code.

## Current corpus

The manifest is checked into `Private/Tests/K2BlueprintCorpusTests.cpp`. The July 21, 2026 admission pass used local Blueprint-open history, currently opened Perforce files and their revision signal, asset modification time, and fresh adjacent `.T3D` exports. The resulting corpus is deterministic in CI; it does not change according to the current machine's recent-files list.

| Blueprint | Why it is included |
|---|---|
| `BP_LoomLift` | Most recently opened large Loom Blueprint; broad event/function coverage. |
| `BP_LoomModuleItem` | Recently opened and most recently saved core Loom item. |
| `BP_LoomMod_DroneClamp` | Highest local Blueprint-open count in the available editing sessions. |
| `BP_LoomMod_LightCable` | Recently opened and saved module graph. |
| `BP_LoomCradle` | Root of the currently edited Loom feature. |
| `BP_LoomCradleHex` | Current simple-function grid-alignment and inherited/read-only graph repro. |
| `BP_LoomSlotDriver` | Recently saved/currently edited feature driver. |
| `BP_DrinkMeStation` | High-revision, large, carefully hand-formatted production graph. |
| `BP_DrinkMeIngredientSlot` | Recently opened DrinkMe slot graph. |
| `BP_DrinkMeOutputSlot` | Frequently revised sibling slot graph. |
| `BP_WorldItemSlot` | Recently opened large item-slot graph. |
| `BP_WorldItem` | Frequently revised foundational item graph. |
| `BPC_ResourceCarrier` | Current `DropHeldActor` no-op regression with an authored multi-link data bus beneath the execution spine. |

Intermediate compiler graphs and `_MERGED` artifacts are excluded. Every eligible authored event, function, macro, and collapsed K2 graph with at least two nodes is exercised.

## Acceptance gates

Each graph is duplicated into `/Engine/Transient` and tested with fixed Preserve Human Layout settings: a 128-unit visible major-grid cell, hybrid snapping, 160-unit execution spacing, 96-unit node/branch spacing, and 256-unit component spacing.

For **Format Graph**, the corpus requires:

- identical node count and exact physical pin-link topology;
- no increase in node overlaps, backward execution edges, preferred execution bends, or execution crossings, and no material regression in backward data flow or data crossings;
- no material increase in wires passing beneath unrelated nodes;
- no newly introduced sub-cell gap on a directly connected, straight primary execution edge (an unchanged authored short gap is preserved rather than misreported as formatter damage);
- execution-root horizontal and vertical drift limited to half a coarse cell, with root reading order unchanged;
- graphs with no measured readability improvement to keep at least 65% of nodes within one coarse cell of their authored position; an improving candidate may move at most 65% beyond a cell, with a three-node allowance so a small exec/data chain can be straightened without forcing Full Reflow;
- a second pass to move zero nodes, resize zero comments, and preserve topology.

`BPC_ResourceCarrier.DropHeldActor` additionally requires a genuinely formatted result with useful node movement. A readability-gate no-op is a failure for this targeted regression, not an acceptable “safe” result.

For **Format + Route Wires**, generated Graph Formatter knots are collapsed while topology is compared. The real endpoint topology and every authored node must remain identical; overlaps, backward data flow, execution/data crossings, and wire-under-node counts cannot materially regress; and a second pass may not move nodes, create more knots, or grow the graph.

The harness fingerprints every source graph's authored node positions, dimensions, and pin links before testing and verifies that state plus the source package dirty flag afterward.

The automation geometry is intentionally deterministic and headless: persisted node dimensions are used when available, with the same adapter fallback sizes and ordered pin offsets used by the formatter. This covers asset loading, the K2 adapter, layout, routing, topology, stability, and source immutability. Screenshot/Slate-geometry goldens remain a complementary visual test because a headless metric cannot fully decide whether a graph “looks human.”

## Refreshing the corpus

1. Use `p4-list-opened` to identify Blueprints currently being worked on.
2. Review recent `LogAssetEditorSubsystem: Opening Asset editor for Blueprint` entries in the local Labrador editor logs. Count repeated opens and retain the latest occurrence.
3. Prefer assets that add new graph motifs or materially more complex hand-authored layout; do not add a dozen near-identical child Blueprints just because they share one changelist.
4. Confirm the adjacent `.T3D` exists and is at least as new as the `.uasset`. If not, run `export-asset-text` on the narrowest practical `/Game` folder and review the export before admission.
5. Add a stable object path, descriptive test label, and selection evidence to `BlueprintCorpus`.
6. Build the editor, run the corpus filter, and inspect every before/after metric line. A threshold should only be changed when the human-readable rule changed—not merely to bless a new regression.
7. Keep visual before/after captures for any graph whose accepted output moves substantially. Promote a representative capture to a geometry/screenshot golden when it exposes a failure that the numeric gates cannot distinguish.

Assets may leave the corpus when they are deleted or superseded, but an unloadable path is a test failure rather than an automatic skip. That makes renames and accidental coverage loss visible.
