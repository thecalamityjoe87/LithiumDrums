# Models

Real meshes that `assets/blender-kit.py` loads for the pieces it can't build from shapes.

| Files | What | Source | Licence |
| --- | --- | --- | --- |
| `hand_flat_*`, `hand_flat.json` | An open hand, for the hand clap | MakeHuman body through the MPFB extension for Blender, posed and cut down | CC0 |
| `hand_snap_*`, `hand_snap.json` | The finger snap | The same body, posed by `pose-snap.py` | CC0 |
| `rubber_boots/` | The boot in the foot stomp | Poly Haven, Rubber Boots | CC0 |

Each hand comes as `_hand.obj` (to the wrist) and `_full.obj` (with the forearm).
`crop-layers.py` uses the hand-only render to find where the hand ends, and fades
the forearm out from there. The `.json` holds the rest-pose wrist, finger and palm
directions, which `load_hand()` uses to turn the hand into place.

## Posing the snap

`pose-snap.py` needs the MPFB extension installed in Blender:

    blender -b --python assets/models/pose-snap.py -- out=assets/models

Its docstring explains the solver and the limits on it. After changing the pose,
render the snap again (`mode=pieces only=snap`) and crop it.

The script for the flat hand wasn't kept, so `hand_flat_*` can't be regenerated
from here.
