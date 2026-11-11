# FalconFOC V1 CLI

The codec, JSON CLI, shared-vector tests and simulated connection use Python's
standard library. A selected physical port uses the existing pyserial package.
No project dependency manifest is changed.

Run from the repository root.

```powershell
cmake -S firmware/at32f415 -B build/sw04-host -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/sw04-host
python -m unittest discover -s tools/falconfoc-cli -p test_falconfoc_cli.py -v
python tools/falconfoc-cli/falconfoc_cli.py encode hello --sequence 0
python tools/falconfoc-cli/falconfoc_cli.py decode --hex 06464601010101010101010101010101038ce700
python tools/falconfoc-cli/falconfoc_cli.py vectors
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/foc-serial-sim.exe demo
python tools/falconfoc-cli/falconfoc_cli.py --simulator build/sw04-host/foc-serial-sim.exe request param-read --id 1
```

Every response prints a JSON object. The simulator exchanges binary pipe bytes
with the actual C protocol, UART buffers and Runtime adapter. Its clock is fixed
at zero and it has no valid sensor measurements. Real-time timeout scenarios
are covered by the C tests with advancing fake time.

On a separately authorized hardware session, `--port COM3 request status`
selects the physical transport, automatically performs HELLO and sends the
requested operation. That command opens the port. This implementation has only
been exercised with the C simulator.

Command construction examples

```powershell
python tools/falconfoc-cli/falconfoc_cli.py encode setpoint --session 1 --sequence 3 --first 0.1 --second 0.2
python tools/falconfoc-cli/falconfoc_cli.py encode setpoint --session 1 --sequence 4 --mode velocity --first 2
python tools/falconfoc-cli/falconfoc_cli.py encode param-write --session 1 --sequence 5 --id 2 --type f32 --value 1
```

The device rejects ARM/START with OUTPUT_LOCKED. A setpoint acceptance reports
its Runtime publication identity. Query status for H0 observation, faults and
the software result. Physical applied is always false in the locked image.
V1 mode selection is constrained to the current Runtime mode. Parameter IDs,
response layouts and session semantics are specified in the
[protocol documentation](../../protocol/README.md).
