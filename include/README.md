# Include Root

`include/` is the shared public-header root for the current FalconFoc repo.

Read first:
- `tm_errno.h`
- `tmdef.h`
- `tmconfig.h`
- `config/tm_core_config.h`

Current ownership:
- common types, error codes, and repo-level config surface
- aggregate compatibility include for build, profile, and board facts

Current non-ownership:
- no active FalconFoc `device/` or `drivers/` runtime model lives here
- product shell behavior belongs under `applications/`
- subsystem mechanism contracts belong under `subsys/`
