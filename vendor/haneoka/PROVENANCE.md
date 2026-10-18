# Haneoka Gekisou Engine Vendor Provenance

- **Upstream Repository**: https://github.com/haneoka-gakuen/haneoka
- **Pinned Commit**: `da667bc20711161538d842164d3dcdfd899b33f9`
- **Upstream Source Paths**:
  - `src/lib/team-builder/engine/*`
  - `src/lib/team-builder/engine/full/*`
  - `src/lib/team-builder/data.ts` (helper subset)
- **License**: Mozilla Public License Version 2.0 (MPL-2.0). See [LICENSE](LICENSE).
- **Execution Environment**: Runs natively on Node.js 24 (`--experimental-transform-types`) using the thin loader in `loader.mjs` with zero npm package dependencies.
- **Main Project License Exception**: While the main repository is licensed under the MIT License, all files within this `vendor/haneoka/` directory remain licensed under MPL-2.0.
