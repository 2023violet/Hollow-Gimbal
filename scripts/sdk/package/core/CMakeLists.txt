# FalconFOC portable Core source target for the SDK.
add_library(
  falconfoc-core STATIC
  src/foc_core.c
  src/foc_current.c
  src/foc_impedance.c
  src/foc_impedance_feasibility.c
  src/foc_impedance_feedforward.c
  src/foc_impedance_tuning.c
  src/foc_motion.c
  src/foc_motion_limits.c
  src/foc_following_error.c
  src/foc_trajectory.c
)
target_include_directories(falconfoc-core PUBLIC include)
target_compile_features(falconfoc-core PUBLIC c_std_11)
set_target_properties(
  falconfoc-core
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)

if(NOT WIN32)
  target_link_libraries(falconfoc-core PUBLIC m)
endif()
