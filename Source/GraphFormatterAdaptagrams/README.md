# GraphFormatter Adaptagrams boundary

This editor module isolates the LGPL-2.1-or-later Adaptagrams implementation from the MIT GraphFormatter module in a
separate Unreal module DLL. It currently embeds `libavoid` solely for comparative, object-avoiding orthogonal routing.

- Upstream: <https://github.com/mjwybrow/adaptagrams>
- Pinned revision: `840ebcff20dbba36ad03a2160edf7cbaf9859984`
- License: `Private/ThirdParty/Adaptagrams/LICENSE.LGPL-2.1`
- Local changes to upstream files: one source-compatible MSVC portability fix in `hyperedgeimprover.cpp` makes an
  upstream implicit pointer-to-`bool` conversion explicit. The expression has identical truth semantics.

The MIT module communicates through `IGraphFormatterAdaptagramsModule`; no Adaptagrams types cross the DLL boundary.
