/* Keep the program image below the two Board1 persistent-settings slots (top 4 KiB). */
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x0801F000, "FalconFOC V2 image reaches the settings slots")
