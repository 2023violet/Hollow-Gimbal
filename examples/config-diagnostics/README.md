# FalconFOC configuration diagnostics example

This standalone C11 consumer links the public `falconfoc-config-diagnostics`,
`falconfoc-config-store-host`, and `falconfoc-virtual-motor` targets. It creates
two committed records in the Host storage model, then passes only its read
callback to the inspector. The output shows the storage manager's selected
slot, format and schema, CRC, commit status, and applicability.

The example lists both slots' motion protection settings by stable Parameter
ID, using the public descriptors for names and units. It also changes the
trusted encoder CPR and shows why a CRC-valid record cannot be applied. A
coherent snapshot of the old Host Runtime shows that both protections remain
disabled. A fresh Runtime initialized with the selected candidate shows matching
effective values and confirmed software INHIBIT with zero UPDATEs. The example
verifies that all 512 storage bytes and the observed state of the old Runtime
are unchanged after diagnostics.

## Build and run

```powershell
cmake -S examples/config-diagnostics -B build/config-diagnostics -G Ninja
cmake --build build/config-diagnostics --target falconfoc-config-diagnostics-demo
ctest --test-dir build/config-diagnostics -R '^test-falconfoc-config-diagnostics-demo$' --output-on-failure
& ./build/config-diagnostics/falconfoc-config-diagnostics-demo.exe
```

To consume the example outside the repository, copy this directory and pass
`-DFALCONFOC_SOURCE_DIR=<FalconFOC source tree>` when configuring the copy. The
source tree can be a copy without Git metadata. The executable has no `.exe`
suffix on platforms that use unsuffixed executables.

The storage and Runtime outputs have `HOST_STORAGE_IMAGE` and `HOST_RUNTIME`
source labels. The example uses simulated storage and virtual motor output.
