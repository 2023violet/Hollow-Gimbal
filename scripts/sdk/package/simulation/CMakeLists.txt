# Host SDK targets. Use the portable Core once, and keep Board/vendor code outside.
add_library(
  falconfoc-pipeline STATIC
  ../runtime/src/foc_pipeline.c
  ../runtime/src/foc_runtime.c
  ../g3/np/src/foc_device.c
  ../g3/np/src/foc_device_sim.c
  ../g3/safe/src/foc_safe.c
)
target_include_directories(
  falconfoc-pipeline PUBLIC ../runtime/include ../g3/np/include ../g3/safe/include
)
target_link_libraries(falconfoc-pipeline PUBLIC falconfoc-core)

add_library(falconfoc-encoder STATIC ../sensors/src/foc_encoder.c)
target_include_directories(falconfoc-encoder PUBLIC ../sensors/include)
target_compile_features(falconfoc-encoder PUBLIC c_std_11)
if(NOT WIN32)
  target_link_libraries(falconfoc-encoder PUBLIC m)
endif()

add_library(
  falconfoc-calibration STATIC
  ../calibration/src/foc_calibration.c ../calibration/src/foc_encoder_calibration.c
  ../calibration/src/foc_mechanical_id.c ../calibration/src/foc_motor_id.c
)
target_include_directories(falconfoc-calibration PUBLIC ../calibration/include)
target_link_libraries(falconfoc-calibration PUBLIC falconfoc-encoder)

add_library(falconfoc-virtual-motor STATIC src/foc_virtual_current.c src/foc_virtual_motor.c)
target_include_directories(falconfoc-virtual-motor PUBLIC include)
target_link_libraries(falconfoc-virtual-motor PUBLIC falconfoc-pipeline falconfoc-encoder)
set_target_properties(
  falconfoc-pipeline falconfoc-encoder falconfoc-calibration falconfoc-virtual-motor
  PROPERTIES C_STANDARD 11
             C_STANDARD_REQUIRED YES
             C_EXTENSIONS OFF
)
