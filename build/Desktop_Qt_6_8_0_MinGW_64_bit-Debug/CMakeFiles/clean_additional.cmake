# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "Debug")
  file(REMOVE_RECURSE
  "CMakeFiles\\pyro_stand_autogen.dir\\AutogenUsed.txt"
  "CMakeFiles\\pyro_stand_autogen.dir\\ParseCache.txt"
  "pyro_stand_autogen"
  )
endif()
