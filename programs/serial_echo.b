Serial port demo (registers are described in the hw header)
Sends "Hi" through the serial device then prints whatever comes back
With Devices / Serial port set to Loopback the bytes return immediately
With a real COM port connect TX to RX on the cable for the same effect

Registers relative to the start cell S:
  SER_STATUS at S minus 15   SER_DATA at S minus 16   SER_TX at S minus 17

<<<<<<<<<<<<<<<<                         ptr SER_DATA
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++   data = 72 which is H
<+                                       write SER_TX: send
>[-]                                     data = 0
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++   data = 105 which is i
<+                                       send
>>                                       ptr SER_STATUS
[                                        while a byte is waiting
  +                                      pop it into SER_DATA
  <.>                                    print it
]
