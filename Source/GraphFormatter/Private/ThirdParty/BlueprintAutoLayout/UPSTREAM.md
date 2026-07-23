# Blueprint Auto Layout vendoring record

- Upstream: <https://github.com/ibrews/blueprint-auto-layout>
- Pinned revision: `de8394f2f83cfc02e4595c6c15e2eb655fae1c55`
- Version: `0.6.9`
- License: MIT (`LICENSE`)
- Imported files: `BlueprintAutoLayout.{h,cpp}` and `BPALLayeredLayout.{h,cpp}`
- Local changes to upstream files: one optional host-supplied pin-Y resolver hook. The benchmark uses
  it to feed the source Blueprint panel's measured pin offsets into transient comparison copies; the
  upstream deterministic ordinal estimate remains the fallback when a pin was not captured.

Only the callable layout implementation is embedded. The upstream toolbar, commands, settings object, and module startup
are deliberately excluded so the benchmark cannot alter the user's editor shortcuts or add a competing toolbar.
