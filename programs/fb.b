Framebuffer demo (registers are described in the hw header): paints the whole 128 x 64 screen with a
colour gradient by incrementing FB_COLOR before every plot
FB_PLOT advances the cursor and wraps to the next row by itself

Registers relative to the start cell S: FB_COLOR at S minus 22   FB_PLOT at S minus 23
Cells: S1 = row counter 64   S2 = column counter 128

>++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++   S1 = 64
[
>++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++   S2 = 128
[
<<<<<<<<<<<<<<<<<<<<<<<<+        FB_COLOR increments (24 left of S2)
<+                               FB_PLOT
>>>>>>>>>>>>>>>>>>>>>>>>>        back to S2
-
]
<-
]
