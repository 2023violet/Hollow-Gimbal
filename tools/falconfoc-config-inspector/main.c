/**
 * @file    main.c
 * @brief   Read-only Host command line for two-slot configuration snapshots.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * File I/O feeds exactly two fixed-size slots to the public diagnostics API.
 * The in-memory driver exposes only a read callback. Runtime is unobserved.
 */

/* Includes --------------------------------------------------------------------- */
#include "diagnostics_output.h"
#include "fixture_profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Private Macros --------------------------------------------------------------- */
#define INSPECTOR_PROFILE "host-fixture-v1"
#define IMAGE_BYTES       (2U * FOC_CONFIG_STORE_SLOT_BYTES)

/* Private Types ---------------------------------------------------------------- */
struct inspector_options {
    const char *profile;
    const char *inputs[2];
    size_t count;
    bool json;
    bool against_default;
};

/* Private Helpers -------------------------------------------------------------- */
static void usage(FILE *stream)
{
    fputs(
        "Usage:\n"
        "  falconfoc-config-inspector inspect [--json] [--profile " INSPECTOR_PROFILE "] IMAGE\n"
        "  falconfoc-config-inspector diff [--json] --profile " INSPECTOR_PROFILE " IMAGE [IMAGE]\n"
        "  falconfoc-config-inspector diff [--json] --profile " INSPECTOR_PROFILE
        " --against-default IMAGE\n"
        "  falconfoc-config-inspector explain [--json] REASON\n",
        stream
    );
}

static bool parse_options(int argc, char **argv, struct inspector_options *options)
{
    bool end_options = false;
    for (int index = 2; index < argc; ++index) {
        const char *argument = argv[index];
        if (!end_options && strcmp(argument, "--") == 0) {
            end_options = true;
        } else if (!end_options && strcmp(argument, "--json") == 0 && !options->json) {
            options->json = true;
        } else if (
            !end_options && strcmp(argument, "--against-default") == 0 && !options->against_default
        ) {
            options->against_default = true;
        } else if (!end_options && strcmp(argument, "--profile") == 0 && options->profile == NULL) {
            if (++index == argc || strcmp(argv[index], INSPECTOR_PROFILE) != 0) {
                fputs("Expected a supported --profile (" INSPECTOR_PROFILE ").\n", stderr);
                return false;
            }
            options->profile = INSPECTOR_PROFILE;
        } else if ((!end_options && argument[0] == '-') || options->count == 2U) {
            fprintf(stderr, "Unexpected or repeated argument: %s\n", argument);
            return false;
        } else {
            options->inputs[options->count++] = argument;
        }
    }
    return true;
}

static bool regular_file(const char *path)
{
#ifdef _WIN32
    struct _stat64 metadata;
    return _stat64(path, &metadata) == 0 && (metadata.st_mode & _S_IFMT) == _S_IFREG;
#else
    struct stat metadata;
    return stat(path, &metadata) == 0 && S_ISREG(metadata.st_mode);
#endif
}

static int
read_image_slot(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size)
{
    const uint8_t *image = context;
    if (slot >= 2U || offset > FOC_CONFIG_STORE_SLOT_BYTES ||
        size > FOC_CONFIG_STORE_SLOT_BYTES - offset) {
        return -1;
    }
    memcpy(bytes, image + slot * FOC_CONFIG_STORE_SLOT_BYTES + offset, size);
    return 0;
}

static bool inspect_image(
    const char *path,
    const struct foc_config_store_policy *policy,
    struct foc_config_diagnostic *report
)
{
    FILE *file = NULL;
    if (regular_file(path)) {
#ifdef _MSC_VER
        (void)fopen_s(&file, path, "rb");
#else
        file = fopen(path, "rb");
#endif
    }
    if (file == NULL) {
        fprintf(stderr, "Cannot open a regular image file for reading: %s\n", path);
        return false;
    }

    uint8_t image[IMAGE_BYTES];
    const size_t read = fread(image, 1U, sizeof(image), file);
    const int trailing = read == sizeof(image) ? fgetc(file) : EOF;
    const bool read_error = ferror(file) != 0;
    const int closed = fclose(file);
    if (read_error || closed != 0) {
        fprintf(stderr, "Cannot read image: %s\n", path);
        return false;
    }
    if (read != sizeof(image) || trailing != EOF) {
        fprintf(
            stderr,
            "Invalid image size: expected exactly %zu bytes (Slot A then B): %s\n",
            sizeof(image),
            path
        );
        return false;
    }

    const struct foc_config_store_driver reader = {.context = image, .read = read_image_slot};
    struct foc_config_store_workspace workspace;
    (void)foc_config_inspect_pair(&reader, policy, &workspace, report);
    return true;
}

static struct inspector_diff_source
slot_source(const struct foc_config_diagnostic *report, enum foc_config_store_slot slot)
{
    const struct foc_config_slot_diagnostic *record =
        slot != FOC_CONFIG_STORE_SLOT_NONE ? &report->slots[slot] : NULL;
    const int status = record != NULL ? record->status : report->restore_status;
    const char *reason =
        record != NULL ? inspector_slot_reason(record) : inspector_restore_reason(report);
    return (struct inspector_diff_source){
        .source = report->source,
        .slot = slot,
        .reason = reason,
        .input = {
            .config = record != NULL && record->applicable ? &record->candidate.config : NULL,
            .status = status,
        },
    };
}

static int run_diff(
    const struct inspector_options *options,
    const struct foc_config_store_policy *policy,
    const struct foc_config_diagnostic *first
)
{
    struct foc_config_diagnostic second;
    struct inspector_diff_source left;
    struct inspector_diff_source right;
    if (options->count == 2U) {
        if (!inspect_image(options->inputs[1], policy, &second)) {
            return 2;
        }
        left = slot_source(first, first->recovery.active_slot);
        right = slot_source(&second, second.recovery.active_slot);
    } else if (options->against_default) {
        left = slot_source(first, first->recovery.active_slot);
        right = (struct inspector_diff_source){
            .source = FOC_DIAG_SOURCE_DEFAULT_CONFIG,
            .slot = FOC_CONFIG_STORE_SLOT_NONE,
            .reason = "NONE",
            .input = {.config = policy->base, .status = FOC_CONFIG_STORE_OK},
        };
    } else {
        left = slot_source(first, FOC_CONFIG_STORE_SLOT_A);
        right = slot_source(first, FOC_CONFIG_STORE_SLOT_B);
    }

    const size_t capacity = foc_config_diff_count();
    struct foc_config_diff_entry *fields = calloc(capacity, sizeof(*fields));
    if (fields == NULL) {
        fputs("Unable to allocate the parameter diff.\n", stderr);
        return 1;
    }
    size_t count = 0U;
    const int status = foc_config_diff(&left.input, &right.input, fields, capacity, &count);
    if (status == FOC_CONFIG_STORE_OK) {
        inspector_print_diff(&left, &right, fields, count, options->profile, options->json);
    } else {
        fprintf(stderr, "Unable to compare configuration (status=%d).\n", status);
    }
    free(fields);
    return status == FOC_CONFIG_STORE_OK ? 0 : 1;
}

/* Public Functions ------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc < 2) {
        usage(stderr);
        return 2;
    }

    const bool inspect = strcmp(argv[1], "inspect") == 0;
    const bool diff = strcmp(argv[1], "diff") == 0;
    const bool explain = strcmp(argv[1], "explain") == 0;
    struct inspector_options options = {0};
    if (!parse_options(argc, argv, &options)) {
        usage(stderr);
        return 2;
    }
    if ((!inspect && !diff && !explain) || options.count == 0U ||
        (!diff && (options.count != 1U || options.against_default)) ||
        (explain && options.profile != NULL) || (options.against_default && options.count != 1U)) {
        usage(stderr);
        return 2;
    }
    if (explain) {
        if (!inspector_explain_reason(options.inputs[0], options.json)) {
            fprintf(stderr, "Unknown diagnostic reason: %s\n", options.inputs[0]);
            return 2;
        }
        return 0;
    }
    if (diff && options.profile == NULL) {
        fputs("diff requires --profile " INSPECTOR_PROFILE " to qualify both inputs.\n", stderr);
        return 2;
    }

    struct inspector_fixture_profile fixture;
    const struct foc_config_store_policy *policy = NULL;
    if (options.profile != NULL) {
        inspector_fixture_profile_init(&fixture);
        policy = &fixture.policy;
    }
    struct foc_config_diagnostic report;
    if (!inspect_image(options.inputs[0], policy, &report)) {
        return 2;
    }

    if (inspect) {
        inspector_print_diagnostics(&report, options.profile, options.json);
        return 0;
    }
    return run_diff(&options, policy, &report);
}

/* ---------------------------------------- end of file ----------------------------------------- */
