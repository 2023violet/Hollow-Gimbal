/* Keep the program image below the CAL-8 cogging page and the two persistent-settings slots
   (top 6 KiB: 0x0801E800 cogging, 0x0801F000 and 0x0801F800 settings). */
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x0801E800, "FalconFOC V2 image reaches the cogging page and settings slots")
