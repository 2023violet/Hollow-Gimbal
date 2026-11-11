/* Require stack headroom between statics and the vendor script's stack top. */
ASSERT(_estack - _ebss >= 4096, "FalconFOC V2 requires 4 KiB of stack headroom")
