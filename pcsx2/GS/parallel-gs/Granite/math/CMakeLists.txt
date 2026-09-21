# Vendored libretro subset, header-only. The aabb/frustum/interpolation/math/
# transforms TUs are not referenced by the libretro core link closure
# (verified via nm) and are dropped along with their headers; muglm.cpp goes
# with them, since it defines only matrix and quaternion helpers and no matrix
# or quat type appears anywhere in paraLLEl-GS. Only the header vector types
# are used, so this carries include directories and nothing to compile.
add_library(granite-math INTERFACE)

target_include_directories(granite-math INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})