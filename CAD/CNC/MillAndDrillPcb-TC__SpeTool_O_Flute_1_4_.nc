(Exported by FreeCAD)
(Post Processor: grbl_post)
(Output Time:2026-03-11 19:31:35.895896)
(Begin preamble)
G17 G90
G21
(Begin operation: TC: SpeTool O Flute 1/4")
(Path: TC: SpeTool O Flute 1/4")
(TC: SpeTool O Flute 1/4")
(Begin toolchange)
( M6 T2 )
M3 S20000
(Finish operation: TC: SpeTool O Flute 1/4")
(Begin operation: Fixture)
(Path: Fixture)
G54
(Finish operation: Fixture)
(Begin operation: ProfileEdges)
(Path: ProfileEdges)
(ProfileEdges)
(Compensated Tool Path. Diameter: 6.35)
G0 Z5.000
G0 X79.245 Y52.737
G0 Z3.000
G1 X79.245 Y52.737 Z-2.000 F100.000
G2 X80.175 Y50.492 Z-2.000 I-2.245 J-2.245 K0.000 F400.000
G1 X80.175 Y0.000 Z-2.000 F400.000
G2 X77.000 Y-3.175 Z-2.000 I-3.175 J0.000 K0.000 F400.000
G1 X0.000 Y-3.175 Z-2.000 F400.000
G2 X-3.175 Y0.000 Z-2.000 I0.000 J3.175 K0.000 F400.000
G1 X-3.175 Y50.492 Z-2.000 F400.000
G2 X0.000 Y53.667 Z-2.000 I3.175 J-0.000 K0.000 F400.000
G1 X77.000 Y53.667 Z-2.000 F400.000
G2 X79.245 Y52.737 Z-2.000 I-0.000 J-3.175 K0.000 F400.000
G0 Z5.000
G0 Z5.000
(Finish operation: ProfileEdges)
(Begin postamble)
M5
G17 G90
M2
