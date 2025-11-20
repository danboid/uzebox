# Source for CP/M binaries

* cpm22.asm : Original CP/M 2.2 sources by Digital Research. Use TASM to compile.
* hello.asm: Example assembler program that can be compiled under CP/M using ASM.COM.
* telnet.asm: Launcher for the Uzebox native telnet client.
* uzeconf.asm: Launcher for the Uzenet module configuration native application.

Notes: 
* To compile these sources, the cross-assembler [The Macro Assembler AS / TASM](http://john.ccac.rwth-aachen.de:8000/as/) is required. A migration to ZASM (https://k1.spdns.de/Develop/Projects/zasm/Distributions/) is considered because of its cross-platformn nature. 

# Creating your own CP/M Binaries:
The recommended approach is to use the [Amsterdam Compiler Kit](https://github.com/davidgiven/ack). It is a cross-compiler for old chips and systems and offer many front-ends like C and basic along several backends like the Intel 8080/8085 that is requied for the UzeboxPc.

## Windows
1. Install MSYS2. See the [Uzebox Wiki](https://uzebox.org/wiki/.Windows_Software_Installation#Building_the_Uzebox_repository_using_MSYS2)
2. Open MSYS2 MSYS shell (the purple icon) ***and not*** MSYS UCRT64 shell (the yellow-green one)). The latter will have the build fail for many reason such as filepath lenght issues, argument list too long and sys/wait.h unavailable.
3. Install the dependiencies:

    > pacman -S --needed base-devel git make gcc ninja python lua pkgconf

4. Checkout the projet: 

    > git clone https://github.com/davidgiven/ack.git

    > cd ack
5. Edit the "Makefile and replace the PLATS variable with: 
    
    > PLATS = cpm

6. In the makefile, replace the whole PREFIX section (including ifq and endif) with the following: (underwise it will copy the files in c:/ack)

    > PREFIX = /usr/local

7. Run the build:

     > make

# License:
* CP/M is now free to use, enhance and disctribute: http://www.cpm.z80.de/license.html