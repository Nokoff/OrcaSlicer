# Auto Paint regression fixture

`gnome_regression.mesh` preserves the exact welded triangle mesh produced when the user-supplied model is imported. It exposed Auto Paint's rounded-part and smooth-limb regressions and is used only by the landmark-based segmentation regression test.

- Source file: `gnome.stl`
- Source SHA-256: `508190C9488DAF7511801F292F4DD012EAD73511327FCF23C187B506357C624F`
- Fixture SHA-256: `C2DA8C035600FDADAD3B01D858C1AED8499DF4DDF446C751E17D8384332551C9`
- Geometry: 469,268 vertices and 938,520 triangles, stored losslessly as packed `float32` vertices and `int32` indices

Do not simplify or remesh this fixture: doing so introduces artificial facet boundaries and changes the segmentation problem being tested.
