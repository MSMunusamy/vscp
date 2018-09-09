# vscp2drv_rpigpio.so

This is a Level II driver for the linux raspberry pi gpio interface

## Setup

### _SETUP_PINMODEn=mode

One for each pin. ponmode18 is for pin 18

Values can be INPUT, OUTPUT, PWM_OUTPUT or GPIO_CLOCK. 
Note that only pin 1 (BCM_GPIO 18) supports PWM output and 
only pin 7 (BCM_GPIO 4) supports CLOCK output modes.

This function has no effect when in Sys mode. If you need to change the pin mode, 
then you can do it with the gpio program in a script before you start your program

### _SETUP_PULLUPn=pud

This sets the pull-up or pull-down resistor mode on the given pin, which should be set 
as an input. The parameter pud should be; OFF, (no pull up/down), DOWN (pull to ground) 
or UP (pull to 3.3v) The internal pull up/down resistors have a value of approximately 50KO 
on the Raspberry Pi.

### _SETUP_INITIALn=state

Initial state for output.
ON or 1 or true for a pin a high lever or OFF or 0 or false for a low pin.

## Inputs

Inputs are read in the main loop and events can be sent continuously or on change.

## Outputs

Decision matrix is used to tell what event should be used to set or rest outputs.

_OUTPUT_MATRIXn=flags;prio;class;type;action;action-param;d0;d1;...

rows must be consecutive.

class type index zone subzone  => action

### flags

bit00 - Compare D0
bit01 - Compare D1
bit02 - Compare D2
bit03 - Compare D3 
bit04 - Compare D4
bit05 - Compare D5
bit06 - Compare D6
bit07 - Compare D7
bit08 - 
bit09 -
bit10 - 
bit11 -  
bit12 - 
bit13 -
bit14 - 
bit15 -  
bit16 - 
bit17 -
bit18 - 
bit19 -  
bit20 - 
bit21 -
bit22 - 
bit23 -  
bit24 - 
bit25 -
bit26 - 
bit27 -  
bit28 - 
bit29 -
bit30 - 
bit31 - Enable 

### Actions

#### ON
	action-parameter is comma separated list with pins to turn on (textual or numerical)

#### OFF
	action-parameter is comma separated list with pins to turn on (textual or numerical)

#### PWM
	action parameter is pin,value

#### Frequency (sound)
	action parameter is frequency, duration

#### Shift out
    action-parameter is data byte to shift out
	
#### Shift out from data
    action-parameter is offset to data byte to shift out

#### SPI

#### I2C
	


