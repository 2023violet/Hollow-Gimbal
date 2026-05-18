#!/usr/bin/env bash

if [[ -n "${THEMIS_FIRMWARE_CLI_LIB_LOADED:-}" ]]; then
    return 0
fi
THEMIS_FIRMWARE_CLI_LIB_LOADED=1

readonly THEMIS_CLI_C_RED='\033[0;31m'
readonly THEMIS_CLI_C_GRN='\033[0;32m'
readonly THEMIS_CLI_C_YEL='\033[0;33m'
readonly THEMIS_CLI_C_BLU='\033[0;34m'
readonly THEMIS_CLI_C_CYN='\033[0;36m'
readonly THEMIS_CLI_C_RST='\033[0m'

themis_cli_log_info()    { printf '%b %s\n' "${THEMIS_CLI_C_BLU}[INFO ]${THEMIS_CLI_C_RST}" "$*"; }
themis_cli_log_ok()      { printf '%b %s\n' "${THEMIS_CLI_C_GRN}[OK   ]${THEMIS_CLI_C_RST}" "$*"; }
themis_cli_log_warn()    { printf '%b %s\n' "${THEMIS_CLI_C_YEL}[WARN ]${THEMIS_CLI_C_RST}" "$*"; }
themis_cli_log_error()   { printf '%b %s\n' "${THEMIS_CLI_C_RED}[ERROR]${THEMIS_CLI_C_RST}" "$*"; }
themis_cli_log_section() { printf '\n%b=== %s ===%b\n' "${THEMIS_CLI_C_CYN}" "$*" "${THEMIS_CLI_C_RST}"; }

readonly THEMIS_CLI_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly THEMIS_CLI_PROJECT_ROOT="${THEMIS_CLI_SCRIPT_DIR}"

readonly THEMIS_CLI_PROJECT_NAME="${THEMIS_FIRMWARE_TARGET_NAME:-falcon-foc-at32}"
readonly THEMIS_CLI_BUILD_DIR="${THEMIS_BUILD_DIR:-build}"
readonly THEMIS_CLI_BUILD_TYPE="${THEMIS_BUILD_TYPE:-Debug}"
readonly THEMIS_CLI_DEVDRV_PROFILE="${THEMIS_DEVDRV_PROFILE:-full-diag}"
readonly THEMIS_CLI_PRODUCT_FLAVOR="${THEMIS_PRODUCT_FLAVOR:-carrier-demo}"
readonly THEMIS_CLI_ENABLE_BOARD_BENCH="${THEMIS_ENABLE_BOARD_BENCH:-OFF}"
readonly THEMIS_CLI_PLATFORM_VENDOR="${THEMIS_PLATFORM_VENDOR:-at}"
readonly THEMIS_CLI_PLATFORM_SOC="${THEMIS_PLATFORM_SOC:-at32f415}"
readonly THEMIS_CLI_BUILD_JOBS="${THEMIS_BUILD_JOBS:-}"
readonly THEMIS_CLI_TOOLCHAIN_FILE="${THEMIS_TOOLCHAIN_FILE:-tools/cmake/arm-gcc-toolchain.cmake}"
readonly THEMIS_CLI_OPENOCD_CFG="${THEMIS_OPENOCD_CFG:-tools/openocd/openocd_daplink.cfg}"
readonly THEMIS_CLI_OPENOCD_BIN="${THEMIS_OPENOCD_BIN:-C:/Program Files (x86)/OpenOCD_V2.0.6/bin/openocd.exe}"

readonly THEMIS_CLI_ELF_FILE="${THEMIS_CLI_BUILD_DIR}/output/${THEMIS_CLI_PROJECT_NAME}.elf"
readonly THEMIS_CLI_REPORT_FILE="${THEMIS_CLI_BUILD_DIR}/output/memory_report.txt"

themis_cli_resolve_project_path() {
    local path="$1"

    if [[ "${path}" =~ ^([A-Za-z]:[\\/]|/) ]]; then
        printf '%s\n' "${path}"
        return 0
    fi

    printf '%s\n' "${THEMIS_CLI_PROJECT_ROOT}/${path}"
}

readonly THEMIS_CLI_TOOLCHAIN_PATH="$(themis_cli_resolve_project_path "${THEMIS_CLI_TOOLCHAIN_FILE}")"
readonly THEMIS_CLI_OPENOCD_CFG_PATH="$(themis_cli_resolve_project_path "${THEMIS_CLI_OPENOCD_CFG}")"
readonly THEMIS_CLI_ELF_PATH="$(themis_cli_resolve_project_path "${THEMIS_CLI_ELF_FILE}")"
readonly THEMIS_CLI_REPORT_PATH="$(themis_cli_resolve_project_path "${THEMIS_CLI_REPORT_FILE}")"
readonly THEMIS_CLI_LOG_ROOT="$(themis_cli_resolve_project_path "${THEMIS_CLI_LOG_DIR:-${THEMIS_CLI_BUILD_DIR}/output/cli}")"

THEMIS_CLI_SESSION_LABEL=""
THEMIS_CLI_SESSION_STAMP=""
THEMIS_CLI_BUILD_LOCK_HELD=0
THEMIS_CLI_LAST_STEP_LOG=""
THEMIS_CLI_BUILD_REPAIR_ATTEMPTED=0

themis_cli_display_path() {
    local path="$1"
    local prefix="${THEMIS_CLI_PROJECT_ROOT}/"

    if [[ "${path}" == "${THEMIS_CLI_PROJECT_ROOT}" ]]; then
        printf '.\n'
        return 0
    fi

    if [[ "${path}" == "${prefix}"* ]]; then
        printf '%s\n' "${path#${prefix}}"
        return 0
    fi

    printf '%s\n' "${path}"
}

themis_cli_init_session() {
    local label="$1"

    if [[ -n "${THEMIS_CLI_SESSION_LABEL}" ]]; then
        return 0
    fi

    THEMIS_CLI_SESSION_LABEL="${label}"
    THEMIS_CLI_SESSION_STAMP="$(date +%Y%m%d_%H%M%S)"
    mkdir -p "${THEMIS_CLI_LOG_ROOT}"
}

themis_cli_step_log_path() {
    local step_name="$1"

    printf '%s/%s_%s.log\n' \
        "${THEMIS_CLI_LOG_ROOT}" \
        "${THEMIS_CLI_SESSION_STAMP}" \
        "${step_name}"
}

themis_cli_build_dir_path() {
    printf '%s\n' "${THEMIS_CLI_PROJECT_ROOT}/${THEMIS_CLI_BUILD_DIR}"
}

themis_cli_format_elf_size() {
    local size_line

    size_line="$(arm-none-eabi-size --format=berkeley "${THEMIS_CLI_ELF_PATH}" | tail -n 1)"
    set -- ${size_line}

    if [[ $# -lt 3 ]]; then
        return 1
    fi

    printf 'text=%s  data=%s  bss=%s\n' "$1" "$2" "$3"
}

themis_cli_detect_parallel_jobs() {
    if [[ -n "${THEMIS_CLI_BUILD_JOBS}" ]]; then
        printf '%s\n' "${THEMIS_CLI_BUILD_JOBS}"
        return 0
    fi

    if command -v nproc >/dev/null 2>&1; then
        nproc
        return 0
    fi

    if command -v getconf >/dev/null 2>&1; then
        getconf _NPROCESSORS_ONLN
        return 0
    fi

    printf '4\n'
}

themis_cli_check_tool() {
    local cmd="$1"

    if ! command -v "${cmd}" >/dev/null 2>&1; then
        themis_cli_log_error "Required tool not found on PATH: ${cmd}"
        exit 1
    fi
}

themis_cli_check_file() {
    local path="$1"
    local label="$2"

    if [[ ! -f "${path}" ]]; then
        themis_cli_log_error "${label} not found: ${path}"
        exit 1
    fi
}

themis_cli_check_profile_value() {
    case "${THEMIS_CLI_DEVDRV_PROFILE}" in
        tiny|standard|full-diag)
            ;;
        *)
            themis_cli_log_error "Unsupported THEMIS_DEVDRV_PROFILE='${THEMIS_CLI_DEVDRV_PROFILE}'."
            exit 1
            ;;
    esac
}

themis_cli_check_flavor_value() {
    case "${THEMIS_CLI_PRODUCT_FLAVOR}" in
        carrier-demo|headless-tiny)
            ;;
        *)
            themis_cli_log_error "Unsupported THEMIS_PRODUCT_FLAVOR='${THEMIS_CLI_PRODUCT_FLAVOR}'."
            exit 1
            ;;
    esac
}

themis_cli_check_board_bench_value() {
    case "${THEMIS_CLI_ENABLE_BOARD_BENCH}" in
        ON|OFF|on|off|1|0|true|false|TRUE|FALSE)
            ;;
        *)
            themis_cli_log_error "Unsupported THEMIS_ENABLE_BOARD_BENCH='${THEMIS_CLI_ENABLE_BOARD_BENCH}'."
            exit 1
            ;;
    esac
}

themis_cli_check_build_prerequisites() {
    themis_cli_log_info "Checking build prerequisites..."
    themis_cli_check_tool cmake
    themis_cli_check_tool ninja
    themis_cli_check_tool arm-none-eabi-gcc
    themis_cli_check_profile_value
    themis_cli_check_flavor_value
    themis_cli_check_board_bench_value
    themis_cli_check_file "${THEMIS_CLI_TOOLCHAIN_PATH}" "CMake toolchain file"
    themis_cli_log_ok "Build prerequisites satisfied."
}

themis_cli_check_flash_prerequisites() {
    themis_cli_log_info "Checking flash prerequisites..."
    themis_cli_check_tool cygpath
    themis_cli_check_file "${THEMIS_CLI_OPENOCD_BIN}" "OpenOCD executable"
    themis_cli_check_file "${THEMIS_CLI_OPENOCD_CFG_PATH}" "OpenOCD configuration"
    themis_cli_log_ok "Flash prerequisites satisfied."
}

themis_cli_check_memory_prerequisites() {
    themis_cli_log_info "Checking memory prerequisites..."
    themis_cli_check_tool arm-none-eabi-size
    themis_cli_check_tool arm-none-eabi-nm
    themis_cli_log_ok "Memory prerequisites satisfied."
}

themis_cli_run_logged_step() {
    local step_name="$1"
    shift

    local log_path
    local command_status

    log_path="$(themis_cli_step_log_path "${step_name}")"
    THEMIS_CLI_LAST_STEP_LOG="${log_path}"

    themis_cli_log_section "${step_name}"
    themis_cli_log_info "Log file    : $(themis_cli_display_path "${log_path}")"
    themis_cli_log_info "Command     : $*"

    set +e
    (
        cd "${THEMIS_CLI_PROJECT_ROOT}"
        "$@"
    ) 2>&1 | tee "${log_path}"
    command_status=${PIPESTATUS[0]}
    set -e

    if [[ ${command_status} -ne 0 ]]; then
        themis_cli_log_error "${step_name} failed. See $(themis_cli_display_path "${log_path}")."
        return "${command_status}"
    fi

    themis_cli_log_ok "${step_name} complete."
}

themis_cli_cache_file() {
    printf '%s\n' "${THEMIS_CLI_PROJECT_ROOT}/${THEMIS_CLI_BUILD_DIR}/CMakeCache.txt"
}

themis_cli_cache_value() {
    local key="$1"
    local cache_file
    local line

    cache_file="$(themis_cli_cache_file)"
    if [[ ! -f "${cache_file}" ]]; then
        return 1
    fi

    line="$(grep -E "^${key}:" "${cache_file}" | tail -n 1 || true)"
    if [[ -z "${line}" ]]; then
        return 1
    fi

    printf '%s\n' "${line#*=}"
}

themis_cli_normalize_path() {
    local path="$1"
    local normalized

    normalized="${path//\\//}"
    if [[ "${normalized}" =~ ^/([A-Za-z])/(.*)$ ]]; then
        normalized="${BASH_REMATCH[1]}:/${BASH_REMATCH[2]}"
    fi
    printf '%s\n' "${normalized}" | tr '[:upper:]' '[:lower:]'
}

themis_cli_normalize_comparable_path() {
    local path="$1"

    if [[ -z "${path}" ]]; then
        printf '\n'
        return 0
    fi

    if [[ ! "${path}" =~ ^([A-Za-z]:[\\/]|/) ]]; then
        path="$(themis_cli_resolve_project_path "${path}")"
    fi

    themis_cli_normalize_path "${path}"
}

themis_cli_collect_cache_mismatches() {
    local cache_file
    local value
    local expected_toolchain
    local expected_source_dir
    local expected_build_dir
    local mismatches=()

    cache_file="$(themis_cli_cache_file)"
    if [[ ! -f "${cache_file}" ]]; then
        printf 'missing-cache\n'
        return 0
    fi

    value="$(themis_cli_cache_value 'THEMIS_BUILD_FIRMWARE' || true)"
    if [[ "${value}" != 'ON' ]]; then
        mismatches+=("firmware=${value:-unset}->ON")
    fi

    value="$(themis_cli_cache_value 'THEMIS_BUILD_HOST_TESTS' || true)"
    if [[ "${value}" != 'OFF' ]]; then
        mismatches+=("host-tests=${value:-unset}->OFF")
    fi

    value="$(themis_cli_cache_value 'CMAKE_BUILD_TYPE' || true)"
    if [[ "${value}" != "${THEMIS_CLI_BUILD_TYPE}" ]]; then
        mismatches+=("build-type=${value:-unset}->${THEMIS_CLI_BUILD_TYPE}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_DEVDRV_PROFILE' || true)"
    if [[ "${value}" != "${THEMIS_CLI_DEVDRV_PROFILE}" ]]; then
        mismatches+=("profile=${value:-unset}->${THEMIS_CLI_DEVDRV_PROFILE}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_PRODUCT_FLAVOR' || true)"
    if [[ "${value}" != "${THEMIS_CLI_PRODUCT_FLAVOR}" ]]; then
        mismatches+=("flavor=${value:-unset}->${THEMIS_CLI_PRODUCT_FLAVOR}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_ENABLE_BOARD_BENCH' || true)"
    if [[ "${value}" != "${THEMIS_CLI_ENABLE_BOARD_BENCH}" ]]; then
        mismatches+=("board-bench=${value:-unset}->${THEMIS_CLI_ENABLE_BOARD_BENCH}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_FIRMWARE_TARGET_NAME' || true)"
    if [[ -n "${value}" && "${value}" != "${THEMIS_CLI_PROJECT_NAME}" ]]; then
        mismatches+=("target=${value}->${THEMIS_CLI_PROJECT_NAME}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_PLATFORM_VENDOR' || true)"
    if [[ -n "${value}" && "${value}" != "${THEMIS_CLI_PLATFORM_VENDOR}" ]]; then
        mismatches+=("platform-vendor=${value}->${THEMIS_CLI_PLATFORM_VENDOR}")
    fi

    value="$(themis_cli_cache_value 'THEMIS_PLATFORM_SOC' || true)"
    if [[ -n "${value}" && "${value}" != "${THEMIS_CLI_PLATFORM_SOC}" ]]; then
        mismatches+=("platform-soc=${value}->${THEMIS_CLI_PLATFORM_SOC}")
    fi

    expected_toolchain="$(themis_cli_normalize_comparable_path "${THEMIS_CLI_TOOLCHAIN_PATH}")"
    value="$(themis_cli_cache_value 'CMAKE_TOOLCHAIN_FILE' || true)"
    if [[ -n "${value}" && "$(themis_cli_normalize_comparable_path "${value}")" != "${expected_toolchain}" ]]; then
        mismatches+=("toolchain")
    fi

    expected_source_dir="$(themis_cli_normalize_comparable_path "${THEMIS_CLI_PROJECT_ROOT}")"
    value="$(themis_cli_cache_value 'CMAKE_HOME_DIRECTORY' || true)"
    if [[ -n "${value}" && "$(themis_cli_normalize_comparable_path "${value}")" != "${expected_source_dir}" ]]; then
        mismatches+=("source-dir")
    fi

    expected_build_dir="$(themis_cli_normalize_comparable_path "$(themis_cli_build_dir_path)")"
    value="$(themis_cli_cache_value 'CMAKE_CACHEFILE_DIR' || true)"
    if [[ -n "${value}" && "$(themis_cli_normalize_comparable_path "${value}")" != "${expected_build_dir}" ]]; then
        mismatches+=("build-dir")
    fi

    if [[ ${#mismatches[@]} -eq 0 ]]; then
        return 0
    fi

    printf '%s\n' "${mismatches[@]}"
}

themis_cli_configure_refresh_requires_metadata_reset() {
    local mismatches="$1"
    local line

    while IFS= read -r line; do
        case "${line}" in
            source-dir|build-dir|toolchain)
                return 0
                ;;
        esac
    done <<< "${mismatches}"

    return 1
}

themis_cli_reset_configure_metadata() {
    local build_dir_path
    local reason="$1"

    build_dir_path="$(themis_cli_build_dir_path)"
    if [[ ! -d "${build_dir_path}" ]]; then
        return 0
    fi

    themis_cli_log_warn "Resetting stale CMake configure metadata in ${THEMIS_CLI_BUILD_DIR}."
    if [[ -n "${reason}" ]]; then
        themis_cli_log_warn "Metadata reset reason: $(printf '%s' "${reason}" | paste -sd ',' -)"
    fi

    rm -rf \
        "${build_dir_path}/CMakeCache.txt" \
        "${build_dir_path}/CMakeFiles" \
        "${build_dir_path}/build.ninja" \
        "${build_dir_path}/rules.ninja" \
        "${build_dir_path}/cmake_install.cmake" \
        "${build_dir_path}/compile_commands.json" \
        "${build_dir_path}/.ninja_deps" \
        "${build_dir_path}/.ninja_log"
}

themis_cli_prepare_configure_refresh() {
    local mismatches="$1"

    if [[ -z "${mismatches}" ]]; then
        mismatches="$(themis_cli_collect_cache_mismatches || true)"
    fi

    if ! themis_cli_configure_refresh_requires_metadata_reset "${mismatches}"; then
        return 0
    fi

    themis_cli_reset_configure_metadata "${mismatches}"
}

themis_cli_force_configure() {
    local reason="$1"

    themis_cli_prepare_configure_refresh ""

    themis_cli_run_logged_step \
        "configure" \
        cmake \
        -S . \
        -B "${THEMIS_CLI_BUILD_DIR}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${THEMIS_CLI_BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="${THEMIS_CLI_TOOLCHAIN_FILE}" \
        -DTHEMIS_BUILD_FIRMWARE=ON \
        -DTHEMIS_BUILD_HOST_TESTS=OFF \
        -DTHEMIS_DEVDRV_PROFILE="${THEMIS_CLI_DEVDRV_PROFILE}" \
        -DTHEMIS_PRODUCT_FLAVOR="${THEMIS_CLI_PRODUCT_FLAVOR}" \
        -DTHEMIS_ENABLE_BOARD_BENCH="${THEMIS_CLI_ENABLE_BOARD_BENCH}" \
        -DTHEMIS_PLATFORM_VENDOR="${THEMIS_CLI_PLATFORM_VENDOR}" \
        -DTHEMIS_PLATFORM_SOC="${THEMIS_CLI_PLATFORM_SOC}" \
        -DTHEMIS_FIRMWARE_TARGET_NAME="${THEMIS_CLI_PROJECT_NAME}"

    themis_cli_log_info "Configure reason: ${reason}"
}

themis_cli_ensure_firmware_configured() {
    local mismatches
    local mismatch_summary

    mismatches="$(themis_cli_collect_cache_mismatches || true)"
    if [[ -z "${mismatches}" ]]; then
        themis_cli_log_section "configure"
        themis_cli_log_ok "Reusing matching build tree: ${THEMIS_CLI_BUILD_DIR}"
        return 0
    fi

    mismatch_summary="$(printf '%s' "${mismatches}" | paste -sd ',' -)"
    themis_cli_log_warn "Build tree requires configure refresh: ${mismatch_summary}"
    themis_cli_prepare_configure_refresh "${mismatches}"
    themis_cli_force_configure "${mismatch_summary}"
}

themis_cli_build_step() {
    local cores
    local elf_size
    local build_status
    local build_log

    cores="$(themis_cli_detect_parallel_jobs)"

    if themis_cli_run_logged_step \
        "build" \
        cmake \
        --build "${THEMIS_CLI_BUILD_DIR}" \
        --parallel "${cores}"; then
        build_status=0
    else
        build_status=$?
    fi

    build_log="${THEMIS_CLI_LAST_STEP_LOG}"
    if themis_cli_build_log_has_ninja_metadata_warning "${build_log}"; then
        if [[ ${THEMIS_CLI_BUILD_REPAIR_ATTEMPTED} -eq 0 ]]; then
            THEMIS_CLI_BUILD_REPAIR_ATTEMPTED=1
            themis_cli_repair_ninja_metadata "detected stale Ninja metadata in build output"
            themis_cli_build_step
            return 0
        fi

        themis_cli_log_warn "Ninja metadata warning persisted after one automatic repair pass."
        themis_cli_log_warn "Use ./run.sh rebuild if you need to force a full clean rebuild."
    fi

    if [[ ${build_status} -ne 0 ]]; then
        return "${build_status}"
    fi

    themis_cli_log_build_summary "${build_log}"

    if [[ ! -f "${THEMIS_CLI_ELF_PATH}" ]]; then
        themis_cli_log_error "ELF not produced: $(themis_cli_display_path "${THEMIS_CLI_ELF_PATH}")"
        exit 1
    fi

    themis_cli_log_ok "ELF         : $(themis_cli_display_path "${THEMIS_CLI_ELF_PATH}")"
    if elf_size="$(themis_cli_format_elf_size)"; then
        themis_cli_log_info "Memory      : ${elf_size}"
    else
        themis_cli_log_warn "Unable to summarize ELF size."
    fi
}

themis_cli_flash_step() {
    local elf_windows

    themis_cli_check_file "${THEMIS_CLI_ELF_PATH}" "ELF artifact"
    elf_windows="$(cygpath -m "${THEMIS_CLI_ELF_PATH}")"

    themis_cli_run_logged_step \
        "flash" \
        "${THEMIS_CLI_OPENOCD_BIN}" \
        -f "${THEMIS_CLI_OPENOCD_CFG_PATH}" \
        -c "init" \
        -c "reset halt" \
        -c "program \"${elf_windows}\" verify" \
        -c "reset run" \
        -c "exit"
}

themis_cli_clean_step() {
    themis_cli_log_section "clean"
    if [[ -d "${THEMIS_CLI_PROJECT_ROOT}/${THEMIS_CLI_BUILD_DIR}" ]]; then
        rm -rf "${THEMIS_CLI_PROJECT_ROOT:?}/${THEMIS_CLI_BUILD_DIR}"
        themis_cli_log_ok "Removed build tree: ${THEMIS_CLI_BUILD_DIR}"
    else
        themis_cli_log_info "Nothing to clean."
    fi
}

themis_cli_build_log_has_no_work_message() {
    local log_path="$1"

    if [[ ! -f "${log_path}" ]]; then
        return 1
    fi

    grep -Eq 'ninja: no work to do\.' "${log_path}"
}

themis_cli_build_log_has_ninja_metadata_warning() {
    local log_path="$1"

    if [[ ! -f "${log_path}" ]]; then
        return 1
    fi

    grep -Eq 'ninja: warning: premature end of file; recovering|\.ninja_(deps|log)' "${log_path}"
}

themis_cli_log_build_summary() {
    local log_path="$1"

    if themis_cli_build_log_has_no_work_message "${log_path}"; then
        themis_cli_log_ok "Incremental : build tree already up to date."
    fi
}

themis_cli_remove_ninja_metadata_files() {
    local build_dir_path

    build_dir_path="$(themis_cli_build_dir_path)"
    rm -f \
        "${build_dir_path}/.ninja_deps" \
        "${build_dir_path}/.ninja_log"
}

themis_cli_repair_ninja_metadata() {
    local reason="$1"

    themis_cli_log_warn "Repairing Ninja build metadata: ${reason}"
    themis_cli_remove_ninja_metadata_files
    themis_cli_force_configure "ninja-metadata-repair"
}

themis_cli_build_lock_path() {
    printf '%s\n' "${THEMIS_CLI_PROJECT_ROOT}/${THEMIS_CLI_BUILD_DIR}/.themis-run.lock"
}

themis_cli_release_build_lock() {
    local lock_path

    if [[ ${THEMIS_CLI_BUILD_LOCK_HELD} -eq 0 ]]; then
        return 0
    fi

    lock_path="$(themis_cli_build_lock_path)"
    rmdir "${lock_path}" >/dev/null 2>&1 || true
    THEMIS_CLI_BUILD_LOCK_HELD=0
}

themis_cli_acquire_build_lock() {
    local lock_path

    mkdir -p "${THEMIS_CLI_PROJECT_ROOT}/${THEMIS_CLI_BUILD_DIR}"
    lock_path="$(themis_cli_build_lock_path)"

    if mkdir "${lock_path}" >/dev/null 2>&1; then
        THEMIS_CLI_BUILD_LOCK_HELD=1
        trap themis_cli_release_build_lock EXIT
        return 0
    fi

    themis_cli_log_error "Build tree is already in use: ${THEMIS_CLI_BUILD_DIR}"
    themis_cli_log_error "Wait for the other firmware CLI session to finish, or choose a different THEMIS_BUILD_DIR."
    exit 1
}

themis_cli_elf_tool_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "${THEMIS_CLI_ELF_PATH}"
        return 0
    fi

    printf '%s\n' "${THEMIS_CLI_ELF_PATH}"
}

themis_cli_memory_section_size_bytes() {
    local section_name="$1"
    local elf_tool_path
    local line

    elf_tool_path="$(themis_cli_elf_tool_path)"
    line="$(arm-none-eabi-size -A "${elf_tool_path}" | grep -E "^${section_name}[[:space:]]+" | head -n 1 || true)"
    if [[ -z "${line}" ]]; then
        printf '0\n'
        return 0
    fi

    set -- ${line}
    if [[ $# -lt 2 ]]; then
        printf '0\n'
        return 0
    fi

    printf '%s\n' "$2"
}

themis_cli_print_symbol_table() {
    local title="$1"
    local filter="$2"
    local count="$3"
    local elf_tool_path
    local idx=0
    local addr
    local size
    local type
    local name
    local tmp_file

    elf_tool_path="$(themis_cli_elf_tool_path)"

    themis_cli_log_section "${title}"
    printf "${THEMIS_CLI_C_YEL}%6s  %10s  %4s  %s${THEMIS_CLI_C_RST}\n" "Rank" "Size (B)" "Type" "Symbol Name"
    echo "--------------------------------------------------------------------------"

    tmp_file="$(mktemp)"
    arm-none-eabi-nm --print-size --radix=d "${elf_tool_path}" \
        | grep -E "${filter}" \
        | sort -k2 -n -r \
        | awk -v max_count="${count}" 'NR <= max_count { print }' > "${tmp_file}"

    while read -r addr size type name; do
        [[ -z "${addr}" ]] && continue

        idx=$((idx + 1))
        printf "%6d  %10s  " "${idx}" "${size}"
        case "${type}" in
            T|t)
                printf "%b%s%b   " "${THEMIS_CLI_C_GRN}" "${type}" "${THEMIS_CLI_C_RST}"
                ;;
            D|d|B|b)
                printf "%b%s%b   " "${THEMIS_CLI_C_RED}" "${type}" "${THEMIS_CLI_C_RST}"
                ;;
            R|r)
                printf "%b%s%b   " "${THEMIS_CLI_C_BLU}" "${type}" "${THEMIS_CLI_C_RST}"
                ;;
            *)
                printf "%s   " "${type}"
                ;;
        esac
        printf "%s\n" "${name}"
    done < "${tmp_file}"

    rm -f "${tmp_file}"
    echo
}

themis_cli_memory_step() {
    local elf_tool_path

    themis_cli_check_file "${THEMIS_CLI_ELF_PATH}" "ELF artifact"
    mkdir -p "$(dirname -- "${THEMIS_CLI_REPORT_PATH}")"

    (
        cd "${THEMIS_CLI_PROJECT_ROOT}"
        elf_tool_path="$(themis_cli_elf_tool_path)"

        themis_cli_log_info "Build dir    : ${THEMIS_CLI_BUILD_DIR}"
        themis_cli_log_info "Firmware     : ${THEMIS_CLI_PROJECT_NAME}"
        themis_cli_log_info "Profile      : ${THEMIS_CLI_DEVDRV_PROFILE}"
        themis_cli_log_info "Flavor       : ${THEMIS_CLI_PRODUCT_FLAVOR}"
        themis_cli_log_info "Target ELF   : $(themis_cli_display_path "${THEMIS_CLI_ELF_PATH}")"

        {
            local size_line
            local text_bytes
            local data_bytes
            local bss_bytes
            local flash_bytes
            local ram_bytes
            local user_heap_stack_bytes
            local linker_ram_bytes

            size_line="$(arm-none-eabi-size --format=berkeley "${elf_tool_path}" | tail -n 1)"
            set -- ${size_line}
            text_bytes="${1:-0}"
            data_bytes="${2:-0}"
            bss_bytes="${3:-0}"
            flash_bytes=$((text_bytes + data_bytes))
            ram_bytes=$((data_bytes + bss_bytes))
            user_heap_stack_bytes="$(themis_cli_memory_section_size_bytes '._user_heap_stack')"
            linker_ram_bytes=$((ram_bytes + user_heap_stack_bytes))

            themis_cli_log_section "Footprint Summary"
            echo "  text                 : ${text_bytes} B"
            echo "  data                 : ${data_bytes} B"
            echo "  bss                  : ${bss_bytes} B"
            echo "  flash (text + data)  : ${flash_bytes} B"
            echo "  ram (data + bss)     : ${ram_bytes} B"
            echo "  ._user_heap_stack    : ${user_heap_stack_bytes} B"
            echo "  linker ram total     : ${linker_ram_bytes} B"
            echo

            themis_cli_print_symbol_table "Top 200 Largest Code Symbols (.text)" "[[:space:]][Tt][[:space:]]" 200
            themis_cli_print_symbol_table "Top 20 Largest Data/BSS Symbols (.data/.bss)" "[[:space:]][DdBb][[:space:]]" 20
            themis_cli_print_symbol_table "Top 20 Largest Read-Only Symbols (.rodata)" "[[:space:]][Rr][[:space:]]" 20

            themis_cli_log_section "Analysis Hints"
            echo "  [T/t] code in flash"
            echo "  [R/r] const data in flash"
            echo "  [D/d] initialized RAM owners"
            echo "  [B/b] zero-init RAM owners"
            echo
            echo "  Watch for double-precision helpers such as __aeabi_d* in code symbols."
            echo "  Those usually indicate avoidable soft-float flash cost."
            echo
        } | tee "${THEMIS_CLI_REPORT_PATH}"
    )

    themis_cli_log_ok "Memory report: $(themis_cli_display_path "${THEMIS_CLI_REPORT_PATH}")"
}

themis_cli_print_banner() {
    local title="$1"

    printf '%b%s%b\n' "${THEMIS_CLI_C_CYN}" "${title}" "${THEMIS_CLI_C_RST}"
    echo "  build dir : ${THEMIS_CLI_BUILD_DIR}"
    echo "  firmware  : ${THEMIS_CLI_PROJECT_NAME}"
    echo "  platform  : ${THEMIS_CLI_PLATFORM_VENDOR}/${THEMIS_CLI_PLATFORM_SOC}"
    echo "  profile   : ${THEMIS_CLI_DEVDRV_PROFILE}"
    echo "  flavor    : ${THEMIS_CLI_PRODUCT_FLAVOR}"
    echo "  bench     : ${THEMIS_CLI_ENABLE_BOARD_BENCH}"
}

themis_cli_print_status() {
    local cache_file
    local mismatches

    cache_file="$(themis_cli_cache_file)"
    mismatches="$(themis_cli_collect_cache_mismatches || true)"

    themis_cli_log_section "status"
    themis_cli_log_info "Project root : $(themis_cli_display_path "${THEMIS_CLI_PROJECT_ROOT}")"
    themis_cli_log_info "Build dir    : ${THEMIS_CLI_BUILD_DIR}"
    themis_cli_log_info "ELF path     : $(themis_cli_display_path "${THEMIS_CLI_ELF_PATH}")"
    themis_cli_log_info "Log root     : $(themis_cli_display_path "${THEMIS_CLI_LOG_ROOT}")"

    if [[ -f "${cache_file}" ]]; then
        themis_cli_log_info "Cache        : present"
    else
        themis_cli_log_warn "Cache        : missing"
    fi

    if [[ -z "${mismatches}" ]]; then
        themis_cli_log_ok "Build tree matches the requested firmware configuration."
    else
        themis_cli_log_warn "Build tree requires configure refresh: $(printf '%s' "${mismatches}" | paste -sd ',' -)"
    fi
}

themis_cli_print_help() {
    cat <<EOF
${THEMIS_CLI_C_CYN}run.sh${THEMIS_CLI_C_RST} - Themis firmware build / flash helper

Usage:
  ${THEMIS_CLI_C_GRN}./run.sh${THEMIS_CLI_C_RST} [command]

Commands:
  ${THEMIS_CLI_C_YEL}run${THEMIS_CLI_C_RST}         Ensure cache matches, incremental build, then flash
  ${THEMIS_CLI_C_YEL}all${THEMIS_CLI_C_RST}         Alias of \`run\`
  ${THEMIS_CLI_C_YEL}build${THEMIS_CLI_C_RST}       Ensure cache matches, then incremental build only
  ${THEMIS_CLI_C_YEL}flash${THEMIS_CLI_C_RST}       Flash the last built ELF
  ${THEMIS_CLI_C_YEL}memory${THEMIS_CLI_C_RST}      Generate memory report through ./memory.sh
  ${THEMIS_CLI_C_YEL}configure${THEMIS_CLI_C_RST}   Force a firmware configure refresh
  ${THEMIS_CLI_C_YEL}reconfigure${THEMIS_CLI_C_RST} Alias of \`configure\`
  ${THEMIS_CLI_C_YEL}rebuild${THEMIS_CLI_C_RST}     Clean, configure, and build
  ${THEMIS_CLI_C_YEL}repair${THEMIS_CLI_C_RST}      Refresh Ninja metadata, reconfigure, and rebuild incrementally
  ${THEMIS_CLI_C_YEL}clean${THEMIS_CLI_C_RST}       Remove build directory
  ${THEMIS_CLI_C_YEL}status${THEMIS_CLI_C_RST}      Show build-tree compatibility status
  ${THEMIS_CLI_C_YEL}help${THEMIS_CLI_C_RST}        Print this message

Current defaults:
  Build dir   : ${THEMIS_CLI_BUILD_DIR}
  Build type  : ${THEMIS_CLI_BUILD_TYPE}
  Firmware    : ${THEMIS_CLI_PROJECT_NAME}
  Platform    : ${THEMIS_CLI_PLATFORM_VENDOR}/${THEMIS_CLI_PLATFORM_SOC}
  Profile     : ${THEMIS_CLI_DEVDRV_PROFILE}
  Flavor      : ${THEMIS_CLI_PRODUCT_FLAVOR}
  Log root    : $(themis_cli_display_path "${THEMIS_CLI_LOG_ROOT}")

Environment overrides:
  THEMIS_BUILD_DIR=build
  THEMIS_BUILD_TYPE=Debug|Release|MinSizeRel
  THEMIS_FIRMWARE_TARGET_NAME=falcon-foc-at32
  THEMIS_PLATFORM_VENDOR=at
  THEMIS_PLATFORM_SOC=at32f415
  THEMIS_DEVDRV_PROFILE=tiny|standard|full-diag
  THEMIS_PRODUCT_FLAVOR=carrier-demo|headless-tiny
  THEMIS_ENABLE_BOARD_BENCH=ON|OFF
  THEMIS_BUILD_JOBS=4
  THEMIS_OPENOCD_BIN='C:/Program Files (x86)/OpenOCD_V2.0.6/bin/openocd.exe'
  THEMIS_OPENOCD_CFG=tools/openocd/openocd_daplink.cfg
  THEMIS_CLI_LOG_DIR=build/output/cli
EOF
}
