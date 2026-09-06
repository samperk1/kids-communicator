# The case

Three printed parts. **All three are original work by Sam Perkins**, designed in
FreeCAD — this is not a remix of anyone else's model.

| Part | File | Volume | Notes |
|---|---|---|---|
| Main body | `stl/case-body.stl` | 21,227 mm³ | Holds the display board, speaker and pack |
| Back plate | `stl/back-plate.stl` | 19,485 mm³ | |
| Display cover | `stl/display-cover.stl` | 15,158 mm³ | |

All three export as **closed, valid solid meshes**.

## What is deliberately NOT here

The FreeCAD document contains four **mockup solids** used only as fit
references. None of them is a printed part and none is exported:

- `PCB (Solid)` — the **manufacturer's 3D model of the ESP32-S3 display board**.
  The only piece of third-party geometry anywhere in the project.
- `battery mockup`, `speaker mock`, `pcm mockup` — Sam's own stand-ins for the
  pack, the speaker and the PCM5102A board.

## ⚠️ If you open the FreeCAD source, recompute before you export

The saved document carries **stale computed shapes**. Opened as-is, the main
body reports `isValid() == False` — 13 of its 114 faces are bad, the feature
tree goes wrong at `Fillet017`, and the errors read `Self-intersecting wire`
and a run of `Unorientable shape`. An STL exported from that state is **not a
solid mesh**.

None of that is a real modelling error. Forcing a full recompute (touch every
object, then recompute) clears all 13 bad faces, the body comes back valid, and
the exported mesh is solid. The volume also shifts very slightly — 21,247 mm³
stale versus 21,227 mm³ recomputed — so the stale shape really was out of date,
not merely flagged.

**The STLs in `stl/` were exported from the recomputed geometry**, so they are
correct as shipped. This note matters only if you regenerate them yourself.
