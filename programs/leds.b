LED chaser driven through the memory mapped hardware (see src/devices.h)

The start cell is S (index 256) and the device window is to its left:
  LEDS at S minus 1   TICKS1 at S minus 6   HALT at S minus 11   TICK_LATCH at S minus 13
Cells used: S0 pattern  S1 counter  S2 saved time  S3 wait flag  S4 scratch  S5 scratch
Each step waits for TICKS1 (units of 256 ms) to change then rotates the lamp

+                        S0 = 1                                   ptr S0
>++++++++++++++++        S1 = 16                                  ptr S1 (frame count)
[
<                                                                 ptr S0
<[-]>                    LEDS = 0                                 ptr S0
[<+>-]                   S0 to LEDS                               ptr S0
<[>+>>>>>+<<<<<<-]       LEDS to S0 and S5                        ptr S minus 1
>>>>>>[<<<<<<+>>>>>>-]   S5 back to LEDS                          ptr S5
<<<[-]                   S2 = 0                                   ptr S2
<<<<<<<<<<<<<<<+         write TICK_LATCH                         ptr S minus 13
>>>>>>>                                                           ptr S minus 6
[>>>>>>>>+<<<<<<<<-]     TICKS1 to S2                             ptr S minus 6
>>>>>>>>>+               S3 = 1                                   ptr S3
[
<<<<<<<<<<<<<<<<+        write TICK_LATCH                         ptr S minus 13
>>>>>>>                                                           ptr S minus 6
[>>>>>>>>>>+<<<<<<<<<<-] TICKS1 to S4                             ptr S minus 6
>>>>>>>>                                                          ptr S2
[>>->+<<<-]              S4 = S4 minus S2 while S5 = S2           ptr S2
>>>[<<<+>>>-]            S5 back to S2                            ptr S5
<                                                                 ptr S4
[<->[-]]                 if S4 nonzero: S3 = 0 and S4 = 0         ptr S4
<                                                                 ptr S3
]
<<<                                                               ptr S0
[>>>>++<<<<-]            S4 = 2 x S0                              ptr S0
>>>>[<<<<+>>>>-]         S0 = S4                                  ptr S4
+<<<<                    S4 = 1                                   ptr S0
[[>>>>>+<<<<<-]>>>>-<<<<] if S0 nonzero: S0 to S5 and S4 = 0      ptr S0
>>>>>[<<<<<+>>>>>-]      S5 back to S0                            ptr S5
<[-<<<<+>>>>]            if S4: S4 = 0 and S0 = 1                 ptr S4
<<<-                     S1 decrement                             ptr S1
]
<<<<<<<<<<<<+            write HALT
