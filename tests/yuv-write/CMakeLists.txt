# Acceptance tests for the YUV output of dec265.
#
# yuv-plane-geometry: decodes the fixed sample matrix and asserts the plane
#                     size relations of the image objects (public API level).
# dec265-yuv-write:   runs dec265 -o on the sample matrix and compares the
#                     written files byte-exactly against hardcoded hashes,
#                     and checks the plane size relations of the files.

add_executable (yuv_plane_geometry_test yuv_plane_geometry_test.cc)
target_link_libraries (yuv_plane_geometry_test PRIVATE de265)
target_include_directories (yuv_plane_geometry_test PRIVATE
  "${PROJECT_SOURCE_DIR}/libde265")

add_test (NAME yuv-plane-geometry
  COMMAND yuv_plane_geometry_test "${CMAKE_CURRENT_SOURCE_DIR}/streams")

find_package (Python3 COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
  add_test (NAME dec265-yuv-write
    COMMAND ${Python3_EXECUTABLE} -I
      "${CMAKE_CURRENT_SOURCE_DIR}/check_yuv_write.py"
      $<TARGET_FILE:dec265>
      "${CMAKE_CURRENT_SOURCE_DIR}")
else()
  message (STATUS "Python3 not found: dec265-yuv-write test disabled")
endif()
