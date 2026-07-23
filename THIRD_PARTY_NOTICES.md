# Third-party notices

Graph Formatter's own source remains MIT licensed under [`LICENSE`](LICENSE).

## Blueprint Auto Layout

- Project: <https://github.com/ibrews/blueprint-auto-layout>
- Pinned revision: `de8394f2f83cfc02e4595c6c15e2eb655fae1c55`
- License: MIT
- Vendored license: `Source/GraphFormatter/Private/ThirdParty/BlueprintAutoLayout/LICENSE`
- Scope: callable layout implementation used only by the formatter bakeoff

## Adaptagrams/libavoid

- Project: <https://github.com/mjwybrow/adaptagrams>
- Pinned revision: `840ebcff20dbba36ad03a2160edf7cbaf9859984`
- License: LGPL-2.1-or-later
- Vendored license: `Source/GraphFormatterAdaptagrams/Private/ThirdParty/Adaptagrams/LICENSE.LGPL-2.1`
- Scope: object-avoiding orthogonal routing in the separately built and dynamically loaded
  `GraphFormatterAdaptagrams` editor module

The main MIT module communicates with the Adaptagrams module through Unreal-native value types and a pure module
interface. Adaptagrams implementation types do not cross the DLL boundary.

## elkjs / Eclipse Layout Kernel

- Project: <https://github.com/kieler/elkjs>
- Package: `elkjs@0.12.0`
- Pinned release revision: `ff5771d7165445c42c408bb8a090c8035272218c`
- npm package SHA-1: `7dc1bc71ab8f402d1b6564e2fa509ca1caee276c`
- Upstream license expression: EPL-2.0 OR GPL-3.0-or-later
- Selected license for the unmodified vendored copy: EPL-2.0
- Vendored license: `Resources/ElkJs/package/LICENSE.md`
- Scope: optional, out-of-process ELK Layered layout and orthogonal-routing oracle used only by the formatter
  bakeoff; it is not loaded or linked by the production formatter

The pinned upstream bundle and package metadata are unmodified. GraphFormatter's MIT `run-layout.cjs` launcher
and C++ adapter communicate with it through JSON files.
