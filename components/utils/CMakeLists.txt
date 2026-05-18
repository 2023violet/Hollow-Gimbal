# Themis utils component manifest. Role: - compile generic containers and utility helpers - expose
# only the reviewed utility public include contract - keep this file limited to source, include, and
# dependency truth

set(_utils_public_include_dirs
    "atomic"
    "bitmap"
    "cobs"
    "countdown"
    "crc"
    "deferred_action"
    "debounce"
    "fsm"
    "hash_map"
    "list"
    "mempool"
    "micro_json"
    "mpsc_queue"
    "packer"
    "pct_curve"
    "pubsub"
    "ring_buffer"
    "sched"
    "soft_timer"
    "spsc_queue"
    "stat_welford"
    "tiny_param"
    "tree"
)

themis_add_module(
  NAME
  utils
  BASE_DIR
  ${CMAKE_CURRENT_LIST_DIR}
  GLOBS
  "*/*.c"
  PUBLIC_INCLUDE_DIRS
  ${_utils_public_include_dirs}
  FOLDER
  "themis/components"
)
