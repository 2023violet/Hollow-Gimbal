#!/usr/bin/env bash

set -euo pipefail

readonly THEMIS_RUN_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "${THEMIS_RUN_SCRIPT_DIR}/scripts/lib/themis_firmware_cli.sh"

themis_run_main() {
    local cmd="${1:-run}"

    themis_cli_init_session "run"
    themis_cli_print_banner "Themis firmware run helper"

    case "${cmd}" in
        run|all)
            themis_cli_acquire_build_lock
            themis_cli_check_build_prerequisites
            themis_cli_check_flash_prerequisites
            themis_cli_ensure_firmware_configured
            themis_cli_build_step
            themis_cli_flash_step
            ;;
        build)
            themis_cli_acquire_build_lock
            themis_cli_check_build_prerequisites
            themis_cli_ensure_firmware_configured
            themis_cli_build_step
            ;;
        flash)
            themis_cli_acquire_build_lock
            themis_cli_check_flash_prerequisites
            themis_cli_flash_step
            ;;
        memory)
            exec "${THEMIS_RUN_SCRIPT_DIR}/memory.sh"
            ;;
        configure|reconfigure)
            themis_cli_acquire_build_lock
            themis_cli_check_build_prerequisites
            themis_cli_force_configure "manual-refresh"
            ;;
        rebuild)
            themis_cli_acquire_build_lock
            themis_cli_check_build_prerequisites
            themis_cli_clean_step
            themis_cli_force_configure "clean-rebuild"
            themis_cli_build_step
            ;;
        repair)
            themis_cli_acquire_build_lock
            themis_cli_check_build_prerequisites
            themis_cli_repair_ninja_metadata "manual-repair"
            themis_cli_build_step
            ;;
        clean)
            themis_cli_clean_step
            ;;
        status)
            themis_cli_print_status
            ;;
        help|--help|-h)
            themis_cli_print_help
            ;;
        *)
            themis_cli_log_error "Unknown command: ${cmd}"
            themis_cli_print_help
            exit 1
            ;;
    esac

    themis_cli_log_ok "Done."
}

themis_run_main "$@"
