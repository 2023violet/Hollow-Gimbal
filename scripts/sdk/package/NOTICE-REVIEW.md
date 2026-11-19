# Source and notice review draft

**REDISTRIBUTION BLOCKED — OWNER APPROVAL REQUIRED**

Use the path-level [source manifest](SDK-SOURCE-MANIFEST.json) for provenance and
redistribution status. Git introduction commits identify repository history.
Early Core, Runtime, Device, and Safety files have unresolved upstream origin
for public redistribution review. Later Core additions, Encoder, Calibration,
and Host plant code have project repository history. The example,
documentation, and packaging build entry may come from a local snapshot.

No in-package file has confirmed public redistribution rights in this
candidate. The package's source dependency list contains production FalconFOC
sources and Host/test/example files. Compiler and system libraries are
environment dependencies. The SDK scope omits CMSIS/CMSIS-DSP, Artery startup
and drivers, FreeRTOS, OpenOCD, and board/Legacy firmware. Preserve any upstream
notices and confirm obligations before adding third-party files.

The project owner needs to confirm the copyright holders and inherited
contributions, approve the precise source range and project license,
and review any applicable notice text. A public candidate needs approval
bound to its exact version, commit, and archive digest.
