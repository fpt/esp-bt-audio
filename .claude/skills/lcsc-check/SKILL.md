---
name: lcsc-check
description: Check that each LCSC part number in the schematics matches its KiCad footprint package (e.g. 1N4148WS/SOD-323 on a SOD-123 footprint). Use before a JLCPCB PCBA order, after assigning parts with the JLCPCB tool, or when asked to validate LCSC fields, BOM parts or footprints.
---

# LCSC part vs footprint check

Run from the repo root:

```sh
bash .claude/skills/lcsc-check/check.sh                      # both boards
bash .claude/skills/lcsc-check/check.sh i2s/i2s-power.kicad_sch
```

It exports the BOM from each schematic with `kicad-cli` (fields `Reference, Value,
Footprint, LCSC`), looks up each LCSC number on the public LCSC product API, and prints
one row per group:

- `OK`: the LCSC package name appears in the footprint name (SO/SOP/SOIC treated as equal).
- `CHECK`: names differ. Usually a wrong part (SOD-323 vs SOD-123, 0402 vs 0603), but
  some are naming only (TSOT-23-5 vs SOT-23-5, `SOP-8` vs `SOP-8_3.76x4.96mm`). Compare
  the datasheet land pattern with the footprint before calling it wrong.
- `NOLCSC`: no LCSC field, so JLCPCB won't place it. Expected for THT parts and modules.
- `LOOKUP`: the API returned nothing (bad number or network).

Exit status is 1 if any row is `CHECK` or `LOOKUP`.

## Notes

- The LCSC field must be in the schematic. The JLCPCB tool writes it only to the PCB
  footprints; sync with PCB editor → Tools → Update Schematic from PCB ("Other fields").
- Matching package names doesn't prove the land pattern fits (SOP-8 body widths vary).
  For ICs, compare the datasheet's recommended pads with the footprint.
- Read the description column for things the package check can't see: dielectric (C0G
  vs X7R for audio caps), voltage rating, tolerance.
- The script runs only `kicad-cli` and `curl`; no Python.
