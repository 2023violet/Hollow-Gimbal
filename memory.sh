#!/usr/bin/env bash

set -euo pipefail

readonly THEMIS_MEMORY_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "${THEMIS_MEMORY_SCRIPT_DIR}/scripts/lib/themis_firmware_cli.sh"

themis_memory_main() {
    themis_cli_init_session "memory"
    themis_cli_print_banner "Themis firmware memory helper"
    themis_cli_check_memory_prerequisites
    themis_cli_memory_step
    themis_cli_log_ok "Done."
}

themis_memory_main "$@"
