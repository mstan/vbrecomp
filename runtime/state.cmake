# Use rbengine's standalone snapshot backend, as PSX does for local rewind.
# No networking or scheduler dependency is needed for this lifecycle contract.
set(RECOMP_RBENGINE_ROOT "${VB_RUNTIME_DIR}/../lib/retcomm-rbengine" CACHE PATH "rbengine checkout")
if(NOT EXISTS "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c")
    message(FATAL_ERROR "Initialize lib/retcomm-rbengine, or supply RECOMP_RBENGINE_ROOT.")
endif()
if(NOT TARGET vb_snapshot_ring)
    add_library(vb_snapshot_ring STATIC "${RECOMP_RBENGINE_ROOT}/src/snap/rbe_snap_ring.c")
    target_include_directories(vb_snapshot_ring PUBLIC "${RECOMP_RBENGINE_ROOT}/include")
endif()

# Source, generated code, ABI and compiler identity are strict snapshot gates.
# This intentionally makes no claim of cross-build snapshot support.
file(GLOB _vb_identity_files CONFIGURE_DEPENDS
    "${VB_RUNTIME_DIR}/src/*.c" "${VB_RUNTIME_DIR}/src/*.cpp"
    "${VB_RUNTIME_DIR}/include/*.h" "${VB_RUNTIME_DIR}/include/blip/*.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_vb_identity_files})
set(_vb_identity "${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR};${CMAKE_SIZEOF_VOID_P};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_RELEASE};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_FLAGS};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_BUILD_TYPE};${VBRECOMP_CPUHOOK};${VBRECOMP_DEBUG_TOOLS}")
foreach(_file IN LISTS _vb_identity_files)
    file(SHA256 "${_file}" _hash)
    string(APPEND _vb_identity ";${_hash}")
endforeach()
string(SHA256 VB_STATE_FRAMEWORK_ID "${_vb_identity}")

# Consumers with custom code/state must include that source in the build gate.
# Call after vb_add_runtime_target and pass files relative to the caller.
function(vb_state_add_identity TARGET)
    get_property(_identity TARGET ${TARGET} PROPERTY VB_STATE_ID)
    foreach(_file IN LISTS ARGN)
        get_filename_component(_absolute "${_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(SHA256 "${_absolute}" _hash)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_absolute}")
        string(APPEND _identity ";${_hash}")
    endforeach()
    string(SHA256 _identity "${_identity}")
    set_property(TARGET ${TARGET} PROPERTY VB_STATE_ID "${_identity}")
endfunction()
