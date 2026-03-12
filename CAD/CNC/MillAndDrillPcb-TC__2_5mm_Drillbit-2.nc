(Exported by FreeCAD)
(Post Processor: grbl_post)
(Output Time:2026-03-11 19:31:38.362739)
(Begin preamble)
G17 G90
G21
(Begin operation: TC: 2.5mm Drillbit)
(Path: TC: 2.5mm Drillbit)
(TC: 2.5mm Drillbit)
(Begin toolchange)
( M6 T4 )
M3 S18000
(Finish operation: TC: 2.5mm Drillbit)
(Begin operation: Fixture)
(Path: Fixture)
G54
(Finish operation: Fixture)
(Begin operation: DrillingMountingHoles)
(Path: DrillingMountingHoles)
(DrillingMountingHoles)
(Begin Drilling)
G90
G0 Z5.000
( G98 )
G0 X9.800 Y5.600
G0 Z3.000
(G81 X9.800 Y5.600 Z-2.000 F100.000 R3.000)
G0 X9.800 Y5.600
G1 Z-2.000 F100.00
G0 Z3.000
G0 X9.800 Y44.600
(G81 X9.800 Y44.600 Z-2.000 F100.000 R3.000)
G0 X9.800 Y44.600
G1 Z-2.000 F100.00
G0 Z3.000
G0 X72.800 Y44.600
(G81 X72.800 Y44.600 Z-2.000 F100.000 R3.000)
G0 X72.800 Y44.600
G1 Z-2.000 F100.00
G0 Z3.000
G0 X72.800 Y5.600
(G81 X72.800 Y5.600 Z-2.000 F100.000 R3.000)
G0 X72.800 Y5.600
G1 Z-2.000 F100.00
G0 Z3.000
( G80 )
G0 Z5.000
(Finish operation: DrillingMountingHoles)
(Begin postamble)
M5
G17 G90
M2
