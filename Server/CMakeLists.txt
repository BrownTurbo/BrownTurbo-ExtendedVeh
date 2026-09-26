cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
cmake_policy(SET CMP0115 NEW)

set(COMPONENT_NAME "Extended-Vehicles")
project(${COMPONENT_NAME} LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

if(DEFINED CMAKE_C_COMPILER_ID)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "Unsupported CMAKE_C_COMPILER_ID: '${CMAKE_C_COMPILER_ID}'...")
    endif()
elseif(DEFINED CMAKE_CXX_COMPILER_ID)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "Unsupported CMAKE_CXX_COMPILER_ID: '${CMAKE_CXX_COMPILER_ID}'...")
    endif()
endif()
if(DEFINED CMAKE_GENERATOR)
    if(NOT CMAKE_GENERATOR STREQUAL "Unix Makefiles" AND
       NOT CMAKE_GENERATOR STREQUAL "Ninja" AND
       NOT CMAKE_GENERATOR MATCHES "(Visual Studio)[A-Za-z0-9 ]+")
        message(FATAL_ERROR "Unsupported CMAKE_GENERATOR: '${CMAKE_GENERATOR}'...")
    endif()
endif()
if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
elseif (CMAKE_CXX_STANDARD LESS 20)
    message(FATAL_ERROR "CMAKE_CXX_STANDARD has been set to '${CMAKE_CXX_STANDARD}' which is lower than the minimum required standard (c++20).")
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "component MUST be compiled as 32-bit (x86).")
endif()

string(TOLOWER ${COMPONENT_NAME} COMPONENT_NAME_LOWERCASE)

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
    set(COMPONENT_STRUCTURES ${COMPONENT_STRUCTURES} ${_Temp} PARENT_SCOPE)
endfunction()
### .........
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/src" "*.cpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.cpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/src" "*.hpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.hpp" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/src" "*.h" 1)
ListFilesInSubmodule("${CMAKE_CURRENT_SOURCE_DIR}/../Shared" "*.h" 1)

foreach(PROJECT_FILE IN ITEMS ${COMPONENT_STRUCTURES})
    get_filename_component(FileExt ${PROJECT_FILE} EXT)
    if(FileExt STREQUAL ".hpp" OR FileExt STREQUAL ".h")
        set(COMPONENT_HDR
            ${COMPONENT_HDR}
            ${PROJECT_FILE}
        )
    elseif(FileExt STREQUAL ".cpp" OR FileExt STREQUAL ".c" OR FileExt STREQUAL ".cc")
        set(COMPONENT_SRC
            ${COMPONENT_SRC}
            ${PROJECT_FILE}
        )
    endif()
endforeach()

add_library(${COMPONENT_NAME_LOWERCASE} SHARED)
target_sources(${COMPONENT_NAME_LOWERCASE} PUBLIC ${COMPONENT_SRC} ${COMPONENT_HDR})

add_subdirectory(${CMAKE_SOURCE_DIR}/deps/omp-sdk)
target_include_directories(${COMPONENT_NAME_LOWERCASE} PUBLIC ${CMAKE_SOURCE_DIR}/deps/omp-sdk/include)

file(GLOB PawnNatives_HDR "${CMAKE_SOURCE_DIR}/deps/pawn-natives/*.hpp" "${CMAKE_SOURCE_DIR}/deps/pawn-natives/Internal/*.hpp")
add_library(PawnNatives OBJECT ${PawnNatives_HDR})
set_target_properties(PawnNatives PROPERTIES LINKER_LANGUAGE CXX)
if(DEFINED UNIX)
    target_compile_options(PawnNatives PRIVATE -m32)
    target_link_options(PawnNatives PRIVATE -m32)
endif()
target_include_directories(PawnNatives
    PUBLIC
        ${CMAKE_SOURCE_DIR}/deps/pawn-natives
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/deps>
        $<INSTALL_INTERFACE:>
)
target_link_libraries(PawnNatives PRIVATE OMP-SDK)

file(GLOB SUBHOOK_SRC "${CMAKE_SOURCE_DIR}/deps/subhook/*.c")
file(GLOB SUBHOOK_HDR "${CMAKE_SOURCE_DIR}/deps/subhook/*.h")
if(DEFINED WIN32)
    list(FILTER SUBHOOK_SRC EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/deps/subhook/subhook_unix\\.c$")
else()
    list(FILTER SUBHOOK_SRC EXCLUDE REGEX "${CMAKE_SOURCE_DIR}/deps/subhook/subhook_windows\\.c$")
endif()
add_library(SUBHOOK ${SUBHOOK_SRC} ${SUBHOOK_HDR})
set_target_properties(SUBHOOK PROPERTIES LINKER_LANGUAGE C)
if(DEFINED UNIX)
target_compile_options(SUBHOOK PRIVATE -m32)
target_link_options(SUBHOOK PRIVATE -m32)
endif()
add_compile_definitions(NOMINMAX)
target_compile_definitions(SUBHOOK PUBLIC
  SUBHOOK_IMPLEMENTATION
  SUBHOOK_SEPARATE_SOURCE_FILES
)
target_include_directories(SUBHOOK PUBLIC ${CMAKE_SOURCE_DIR}/deps/subhook)

file(GLOB AMX_SRC "${CMAKE_SOURCE_DIR}/deps/amx/*.c")
if(DEFINED UNIX)
    file(GLOB AMXL_SRC "${CMAKE_SOURCE_DIR}/deps/amx/linux/*.c")
endif()
file(GLOB AMX_HDR "${CMAKE_SOURCE_DIR}/deps/amx/*.h")
if(DEFINED UNIX)
    file(GLOB AMXL_HDR "${CMAKE_SOURCE_DIR}/deps/amx/linux/*.h")
endif()
add_library(AMX ${AMX_SRC} ${AMXL_SRC} ${AMX_HDR} ${AMXL_HDR})
set_target_properties(AMX PROPERTIES LINKER_LANGUAGE CXX)
if(DEFINED UNIX)
target_compile_options(AMX PRIVATE -m32)
target_link_options(AMX PRIVATE -m32)
endif()
target_include_directories(AMX
    PUBLIC
        ${CMAKE_SOURCE_DIR}/deps/amx
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/deps>
        $<INSTALL_INTERFACE:include>
)

file(GLOB RakNet_SRC "${CMAKE_SOURCE_DIR}/deps/RakNet/*.cpp")
file(GLOB RakNetE_SRC "${CMAKE_SOURCE_DIR}/deps/RakNet/Encoding/*.cpp")
file(GLOB RakNet_HDR "${CMAKE_SOURCE_DIR}/deps/RakNet/*.hpp")
file(GLOB RakNetE_HDR "${CMAKE_SOURCE_DIR}/deps/RakNet/Encoding/*.hpp")
add_library(RakNet ${RakNet_SRC} ${RakNetE_SRC} ${RakNet_HDR} ${RakNetE_HDR})
set_target_properties(RakNet PROPERTIES LINKER_LANGUAGE CXX)
if(DEFINED UNIX)
target_compile_options(RakNet PRIVATE -m32)
target_link_options(RakNet PRIVATE -m32)
endif()
target_include_directories(RakNet PUBLIC ${CMAKE_SOURCE_DIR}/deps/RakNet)
target_link_libraries(RakNet PRIVATE OMP-SDK)
target_include_directories(RakNet
    PUBLIC
        ${CMAKE_SOURCE_DIR}/deps/RakNet
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/RakNet>
        $<INSTALL_INTERFACE:>
)

find_package(ZLIB REQUIRED)
find_package(unofficial-hash-library CONFIG REQUIRED)

target_link_libraries(${COMPONENT_NAME_LOWERCASE} PRIVATE PawnNatives OMP-SDK SUBHOOK AMX RakNet ZLIB::ZLIB unofficial::hash-library)
target_link_options(${COMPONENT_NAME_LOWERCASE} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
target_compile_options(${COMPONENT_NAME_LOWERCASE} PRIVATE /p:VcpkgEnableManifest=true)

if(DEFINED UNIX)
    add_definitions(-DLINUX -DENABLE_BINRELOC)

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64)|(x64)|(X64)|(amd64)|(AMD64)" OR CMAKE_SIZEOF_VOID_P EQUAL 8)
        set_property(TARGET ${COMPONENT_NAME_LOWERCASE} PROPERTY FIND_LIBRARY_USE_LIB64_PATHS OFF)
    endif()
    if(DEFINED CMAKE_CXX_COMPILER_ID)
        target_compile_options(${COMPONENT_NAME_LOWERCASE} PRIVATE
            -Wold-style-cast
            -Wctor-dtor-privacy
            -Woverloaded-virtual
            -Wzero-as-null-pointer-constant
            -Wuseless-cast
            -Wsized-deallocation
        )
    endif()
	target_compile_options(${COMPONENT_NAME_LOWERCASE} PRIVATE
		-Wall
		-Wextra
		-pedantic
		-Werror

        -pedantic-errors
        -Wundef
        -Wredundant-decls
        -Wwrite-strings
        -Wpointer-arith
        -Wcast-qual
        -Wformat=2
        -Wmissing-include-dirs
        -Wcast-align
        -Wdisabled-optimization
        -Winvalid-pch
        -Wconversion
        #-Wno-ctor-dtor-privacy
        #-Wno-format-nonliteral
        #-Wno-dangling-else
        #-Wno-unused-local-typedefs
        -Wtrampolines
        -Wvector-operation-performance
        -Wshadow
        -Wdouble-promotion
        -Wshift-overflow=2
        -Wnull-dereference
        -Wduplicated-cond
        -fsanitize=address,leak
        -static-libasan
        -L/usr/deps/i386-linux-gnu
	)
	# link runtime statically
    if(STATIC_BUILD)
        add_link_options(
            "-static-libgcc"
            "-static-libstdc++"
        )
    endif()
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath='$ORIGIN'")
    add_definitions(-D_GLIBCXX_USE_CXX11_ABI=0) # For compatibility with samp03svr that was compiled with an older (< 5.1) version of GCC

    if(MSVC)
        add_compile_options(/wd4820 /wd4365 /wd4242)
    endif()
elseif(DEFINED WIN32)
	if(NOT CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
		message(FATAL_ERROR "Building for Windows requires -A Win32 flag.")
    endif()

    set_property(TARGET ${COMPONENT_NAME_LOWERCASE} PROPERTY POSITION_INDEPENDENT_CODE Off)
	add_definitions(
		-D_CRT_SECURE_NO_WARNINGS
        -DHAVE_STDINT_H
        -DNOGDI # disables ERROR define (conflicts with log level)
		-D_WINSOCK_DEPRECATED_NO_WARNINGS
		-DWin32_LEAN_AND_MEAN
        -DWIN32_LEAN_AND_MEAN
		-D_CRT_DECLARE_NONSTDC_NAMES
		-D_CRT_NONSTDC_NO_WARNINGS
        -DWin32
        -D_Win32
        -D__STDC__
        -DWINDOWS_IGNORE_PACKING_MISMATCH
	)
    if(DEFINED CMAKE_CXX_COMPILER_ID)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Wall /MP /analyze /EHsc /utf-8")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR BUILD_TESTING)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fsanitize=address")
        endif()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
        set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /MTd")
    elseif(DEFINED CMAKE_C_COMPILER_ID)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /Zp4 /Wall /MP /analyze /EHsc /utf-8")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo" OR BUILD_TESTING)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /fsanitize=address")
        endif()
        set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
        set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")
        set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} /MTd")
        set(CMAKE_SHARED_LINKER_FLAGS "/NOLOGO /MACHINE:X86 /INCREMENTAL:NO /NODEFAULTLIB:Win32RT")
    endif()
	# link runtime statically
    if(STATIC_BUILD)
        set(CMAKE_Win32_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

add_definitions(-DSUBHOOK_STATIC -DSUBHOOK_IMPLEMENTATION)
add_definitions(-DHAVE_STDINT_H=1 -DPAWN_CELL_SIZE=64)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++20")
endif()

if(DEFINED WIN32)
    add_custom_command(TARGET ${COMPONENT_NAME_LOWERCASE} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DTRGT=${CMAKE_SOURCE_DIR}/vcpkg_installed/x86-windows/bin"
            "-DDEST=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            -P "${CMAKE_SOURCE_DIR}/CopyRuntimeDlls.cmake"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Resolving and copying runtime DLL dependencies for ${COMPONENT_NAME_LOWERCASE}..."
    )
endif()
