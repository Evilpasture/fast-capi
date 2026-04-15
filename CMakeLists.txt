cmake_minimum_required(VERSION 3.25)
project(fast_capi LANGUAGES C)

# 1. C23 Standard setup
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 2. Dependency Discovery
# Using QUIET so the parent project isn't spammed if it already found Python
find_package(Python REQUIRED COMPONENTS Development.Module QUIET)

# 3. Define the Core Library
# Changed to STATIC or INTERFACE depending on how you want to link, 
# but for a C-extension utility, STATIC is usually safest for submodules.
add_library(fast_capi_core STATIC 
    src/fast_capi/fast_parse.c
)

target_compile_features(fast_capi_core PRIVATE c_std_23)

# 4. Handle Include Paths for Submodule Use
# PUBLIC ensures that when a parent project links to fast_capi_core,
# it automatically knows where to find your headers.
target_include_directories(fast_capi_core PUBLIC 
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${Python_INCLUDE_DIRS}
)

# 5. Link Dependencies
target_link_libraries(fast_capi_core PRIVATE 
    Python::Module
)

# 6. Position Independent Code (Critical for Shared Modules)
# Since this will eventually be linked into a .so / .pyd, 
# the static library MUST be compiled with -fPIC.
set_target_properties(fast_capi_core PROPERTIES 
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
)