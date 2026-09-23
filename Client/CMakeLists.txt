# cmake -B build -A Win32
# cmake --build build --config Release
cmake_minimum_required(VERSION 3.23)
project("brownturbo-chandling" LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL" CACHE STRING "" FORCE)
set(POLYHOOK_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
set(FORCE_STATIC_CRT OFF CACHE BOOL "" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(__BITSTREAM_NATIVE_END)

### .........
function(ListFilesInSubmodule directory_path pattern)
    set(_maxDepth -1)
    if(ARGC GREATER 2)
        set(_maxDepth ${ARGV2})
        if(NOT _maxDepth MATCHES "^[0-9]+$")
            message(FATAL_ERROR "ListFilesInSubmodule: max_depth must be a non-negative integer, got '${_maxDepth}'.")
        endif()
    endif()

    # Get absolute normalized path
    get_filename_component(_baseDir "${directory_path}" ABSOLUTE)

    # Use native CMake globbing (no PowerShell / Bash required)
    file(GLOB_RECURSE _matchedFiles CONFIGURE_DEPENDS RELATIVE "${_baseDir}" "${_baseDir}/${pattern}")

    unset(_Temp)
    foreach(_relFile IN LISTS _matchedFiles)
        # Calculate depth by counting path separators
        string(REGEX MATCHALL "/" _slashes "${_relFile}")
        list(LENGTH _slashes _slashCount)
        math(EXPR _fileDepth "${_slashCount} + 1")

        if(_maxDepth EQUAL -1 OR _fileDepth LESS_EQUAL _maxDepth)
            list(APPEND _Temp "${_baseDir}/${_relFile}")
        endif()
    endforeach()

    if(NOT _Temp)
        set(_maxDepthLog "")
        if(NOT _maxDepth EQUAL -1)
            set(_maxDepthLog " (max_depth=${_maxDepth})")
        endif()
        message(STATUS "No files found matching pattern '${pattern}' in '${directory_path}'${_maxDepthLog}.")
        return()
    endif()

    # Push accumulated files to parent scope
    set(PLUGIN_STRUCTURES ${PLUGIN_STRUCTURES} ${_Temp} PARENT_SCOPE)
endfunction()
### .........

ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}" "*.cpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.cpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}" "*.hpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.hpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}" "*.h" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.h" 1)
add_library(${PROJECT_NAME} SHARED ${PLUGIN_STRUCTURES})

target_compile_definitions(${PROJECT_NAME} PRIVATE
	_NDEBUG
	_CRT_SECURE_NO_WARNINGS
	_CRT_NON_CONFORMING_SWPRINTFS
	GTASA
	GTAGAME_NAME="San Andreas"
	GTAGAME_ABBR="SA"
	GTAGAME_ABBRLOW="sa"
	GTAGAME_PROTAGONISTNAME="CJ"
	GTAGAME_CITYNAME="San Andreas"
	_LA_SUPPORT
	PLUGIN_SGV_10US
	_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
	_DX9_SDK_INSTALLED
	RW
    #WIN32_LEAN_AND_MEAN
    NOMINMAX
    RW_D3D9
	_HAS_STD_BYTE=0
)
add_definitions(
    -D_CRT_SECURE_NO_WARNINGS
    -D_CRT_NONSTDC_NO_DEPRECATE
    -DNOMINMAX
    #-DWIN32_LEAN_AND_MEAN
	-D_HAS_STD_BYTE=0
)
target_compile_options(${PROJECT_NAME} PRIVATE /FIrpc.h /FIwindows.h /FS)

set(PLUGIN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/plugin-sdk")
set(SAMPAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/samp-api")
set(RAKHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/rakhook")

set(PLUGIN_STRUCTURES "")
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/hooking" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/injector" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/modutils" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/plugin_Sa" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/safetyhook" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/shared" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/stb" "*.cpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/hooking" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/injector" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/modutils" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/plugin_Sa" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/safetyhook" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/shared" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/stb" "*.hpp" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/hooking" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/injector" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/modutils" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/plugin_Sa" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/safetyhook" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/shared" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/stb" "*.h" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/hooking" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/injector" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/modutils" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/plugin_Sa" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/safetyhook" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/shared" "*.c" 15)
ListFilesInSubmodule("${PLUGIN_SDK_DIR}/stb" "*.c" 15)

list(FILTER PLUGIN_STRUCTURES EXCLUDE REGEX "Zydis\\.c$")
target_sources(${PROJECT_NAME} PRIVATE ${PLUGIN_STRUCTURES})
add_library(trickedTargets STATIC ${PLUGIN_SDK_DIR}/safetyhook/Zydis.c)
set_target_properties(trickedTargets PROPERTIES LINKER_LANGUAGE C)
target_include_directories(trickedTargets PRIVATE ${PLUGIN_SDK_DIR}/safetyhook)
set_source_files_properties(
    "${PLUGIN_SDK_DIR}/plugin_sa/game_sa/rw/rpdbgerr.c"
    PROPERTIES LANGUAGE CXX
)

set_target_properties(${PROJECT_NAME}
	PROPERTIES
	OUTPUT_NAME ${PROJECT_NAME}
	SUFFIX ".asi"
)

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)
target_compile_options(${PROJECT_NAME} PRIVATE /TP /std:c++latest /Zc:offsetof- /W3 /wd4073 /sdl- /Y- /p:VcpkgEnableManifest=true)
target_compile_definitions(${PROJECT_NAME} PRIVATE _SILENCE_CXX23_ISSUES VALIDATE_OFFSETS=0)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${PLUGIN_SDK_DIR}
    ${PLUGIN_SDK_DIR}/safetyhook
    ${PLUGIN_SDK_DIR}/shared
	${PLUGIN_SDK_DIR}/shared/game
	${PLUGIN_SDK_DIR}/shared/bass
	${PLUGIN_SDK_DIR}/shared/rwd3d9
    ${PLUGIN_SDK_DIR}/plugin_sa
    ${PLUGIN_SDK_DIR}/plugin_sa/game_sa
	${PLUGIN_SDK_DIR}/plugin_sa/game_sa/enums
    ${PLUGIN_SDK_DIR}/plugin_sa/game_sa/rw
    ${PLUGIN_SDK_DIR}/plugin_sa/game_sa/meta
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${PLUGIN_SDK_DIR}/shared/bass/bass.lib
	${PLUGIN_SDK_DIR}/shared/rwd3d9/rwd3d9.lib
	${PLUGIN_SDK_DIR}/shared/dxsdk/d3dx9.lib
)

add_subdirectory(${RAKHOOK_DIR}/lib/minhook)
add_subdirectory(${RAKHOOK_DIR})
add_subdirectory(${SAMPAPI_DIR})

target_include_directories(${PROJECT_NAME} PRIVATE ${SAMPAPI_DIR}/include/sampapi)
target_include_directories(${PROJECT_NAME} PRIVATE ${RAKHOOK_DIR}/lib/minhook/include)
target_include_directories(${PROJECT_NAME} PRIVATE ${RAKHOOK_DIR}/include)

find_package(imgui CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-hash-library CONFIG REQUIRED)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d9
	minhook
    sampapi
    rakhook
	trickedTargets
	imgui::imgui
	d3d9
	ZLIB::ZLIB
	nlohmann_json::nlohmann_json
	unofficial::hash-library
)

function(force_dynamic_runtime_on_all_targets dir)
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(subdir ${subdirs})
        force_dynamic_runtime_on_all_targets(${subdir})
    endforeach()

    get_property(current_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target ${current_targets})
        get_target_property(target_type ${target} TYPE)
        if(NOT target_type STREQUAL "INTERFACE_LIBRARY")
            set_property(TARGET ${target} PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
            )
        endif()
    endforeach()
endfunction()

force_dynamic_runtime_on_all_targets(${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DTRGT=${CMAKE_SOURCE_DIR}/vcpkg_installed/x86-windows/bin"
            "-DDEST=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            -P "${CMAKE_SOURCE_DIR}/CopyRuntimeDlls.cmake"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Resolving and copying runtime DLL dependencies for ${PROJECT_NAME}..."
    )
endif()
