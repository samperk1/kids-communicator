# The case

Three printed parts. **All three are original work by Sam Perkins**, designed in
FreeCAD — this is not a remix of anyone else's model.

| Part | File | Volume | Facets |
|---|---|---|---|
| Main body | `stl/case-body.stl` | 21,227 mm³ | 26,740 |
| Back plate | `stl/back-plate.stl` | 19,485 mm³ | 22,680 |
| Display cover | `stl/display-cover.stl` | 15,158 mm³ | 28,396 |

All three are closed, valid solid meshes.

### Newer revision (2026-10-06)

`stl/complete-case-2026-10-06.stl` is the latest revision of the design, exported
as **one combined mesh** (119 × 56 × 27 mm). It is published as-is so the newest
shape is available; the three separate printable parts and the FreeCAD source
above are the earlier revision and have not yet been re-exported from it.

## Source

`source/kids-communicator-case.FCStd` — FreeCAD 1.0. Modify it freely; that is
why it is here.

**The manufacturer's 3D model of the ESP32-S3 display board has been removed
from this copy.** It was the only piece of third-party geometry in the project,
used purely as a fit reference, and removing it means everything published here
is original work with nothing borrowed to account for.

Removing it changes nothing about the parts. Verified, not assumed: the three
printable bodies keep byte-identical geometry (same volumes, same facet counts)
because the SubShapeBinders that referenced the board had already cached the
geometry they needed. As a bonus the document is now **completely free of
invalid shapes** — the vendor model was the only object in the file that
reported one — and the file dropped from 4.2MB to 1.9MB.

If you are fitting a *different* board, you will want to bring in your own
model and re-cut the openings.

## ⚠️ If you regenerate the STLs, recompute first

FreeCAD can save a document with **stale computed shapes**. When that happens
the main body reports `isValid() == False`, the errors read `Self-intersecting
wire` and `Unorientable shape`, and the exported STL is *not a solid mesh* —
none of which is a real modelling error.

The cure is a full recompute: right-click the document name at the top of the
Model tree, choose **Mark to recompute**, then **Edit → Refresh** (Ctrl+R — it
stays greyed out until something is marked), then save.

The STLs in `stl/` were exported from correctly recomputed geometry.
