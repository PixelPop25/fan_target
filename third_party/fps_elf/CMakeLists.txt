cmake_minimum_required (VERSION 3.20)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(basename "fps_elf")
project(${basename} C CXX ASM)

# Language Standard Defaults
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)

# Check for sub-project as part of main build or external build
if (NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(IS_SUBPROJECT TRUE)
else()
	set(IS_SUBPROJECT FALSE)
endif()

message("IS_SUBPROJECT: ${IS_SUBPROJECT}")

set(D_CWD "${CMAKE_CURRENT_SOURCE_DIR}")
# NOTE: upstream etaHEN points this at ../daemon/assets, a sibling folder
# that only exists in etaHEN's own monorepo layout. fan_target doesn't have
# that folder, which is what was making this fail to configure/build here.
# Output into this project's own bin/ instead.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${D_CWD}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${D_CWD}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${D_CWD}/bin)	# static libs are archive
set(D_SRC ${D_CWD}/src)

# Headers
include_directories	(SYSTEM "${PS5_PAYLOAD_SDK}")
include_directories	(SYSTEM "${PS5_PAYLOAD_SDK}/include")

add_executable(${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "${PROJECT_NAME}.elf")

# Must build with clang
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "[Cc]lang")
	set(IS_CLANG 1)
else()
	message(FATAL_ERROR "${PROJECT_NAME} is meant to be built with clang! CompilerID: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Finalize main target sources
target_compile_options(${PROJECT_NAME} PUBLIC
	$<$<COMPILE_LANGUAGE:C>:${C_DEFS} ${C_FLAGS}>
	$<$<COMPILE_LANGUAGE:CXX>:${CXX_DEFS} ${CXX_FLAGS}>
	$<$<COMPILE_LANGUAGE:ASM>:${ASM_FLAGS}>
)

message("========== build: ${PROJECT_NAME} ==========")


# Standalone runtime-FW-detecting implementation.
# Does NOT depend on V_FW at build time — firmware version is detected at
# runtime via sysctlbyname("kern.osrelease") inside the ELF itself.
set(SrcFiles ${D_SRC}/fps_main.cpp)

# No V_FW define needed. No -Werror / -pedantic so we can build cleanly
# even if the SDK headers emit warnings on this target.
set(CMAKE_C_FLAGS "--target=x86_64-sie-ps5 -DPPR -DPS5 ")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-nested-anon-types -Wno-gnu-anonymous-struct -Wno-zero-length-array -Wno-keyword-macro -fPIC -fPIE -march=znver2 -Wall -fstack-protector-all")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -O2 ")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 ")

target_sources(${PROJECT_NAME} PRIVATE ${SrcFiles})
target_include_directories(${PROJECT_NAME} PRIVATE "${D_CWD}/include" "${D_CWD}/lib")
target_link_directories	(${PROJECT_NAME} PUBLIC "${PROJECT_ROOT}/lib")
target_link_libraries	(${PROJECT_NAME} PUBLIC kernel SceGnmDriver)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Add post-build command
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} --remove-section .debug_info --remove-section .debug_abbrev --remove-section .debug_line --remove-section .debug_str --remove-section .debug_loc --remove-section .debug_aranges --remove-section .debug_ranges --remove-section .debug_pubnames --remove-section .debug_pubtypes --remove-section .debug_frame --strip-unneeded $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Stripping debugging information from ${PROJECT_NAME}.elf"
)

