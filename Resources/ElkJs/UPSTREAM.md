# elkjs benchmark dependency

- Upstream: <https://github.com/kieler/elkjs>
- Package: `elkjs@0.12.0`
- Release commit: `ff5771d7165445c42c408bb8a090c8035272218c`
- npm package SHA-1: `7dc1bc71ab8f402d1b6564e2fa509ca1caee276c`
- License: EPL-2.0 OR GPL-3.0-or-later
- Selected license for this unmodified copy: EPL-2.0
- Vendored files: `package/lib/elk.bundled.js`, `package/LICENSE.md`,
  `package/README.md`, and `package/package.json`
- Local changes to upstream files: none

GraphFormatter invokes this bundle only as an optional, benchmark-only layout
oracle through `run-layout.cjs`. The production formatter does not load or link
elkjs. A `node` executable must be available on `PATH`, or its absolute path can
be supplied through the `GRAPHFORMATTER_ELK_NODE` environment variable.
