#!/usr/bin/env python3
"""Patch etaHEN fps_elf/CMakeLists.txt for a standalone fan_target build."""
import sys
from pathlib import Path

def main():
    if len(sys.argv) < 2:
        print("usage: patch_etahen_fps_cmake.py <CMakeLists.txt>", file=sys.stderr)
        sys.exit(1)
    p = Path(sys.argv[1])
    text = p.read_text()

    # Output into local bin/
    text = text.replace(
        "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${D_CWD}/../daemon/assets)",
        "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${D_CWD}/bin)",
    )

    # Fix GLOB paths that assumed monorepo layout (../../extern from src/)
    text = text.replace("${D_SRC}/../../extern", "${D_CWD}/../extern")
    text = text.replace("${D_SRC}/../../lib", "${D_CWD}/../lib")

    # Expand include dirs
    old_inc = 'target_include_directories(${PROJECT_NAME} PRIVATE "${D_CWD}/include")'
    new_inc = '''target_include_directories(${PROJECT_NAME} PRIVATE
  "${D_CWD}/include"
  "${D_CWD}/../include"
  "${D_CWD}/../lib"
  "${D_CWD}/../extern/tiny-json"
  "${D_CWD}/../extern/cJSON"
)'''
    if old_inc in text:
        text = text.replace(old_inc, new_inc)

    # Link against SDK libs
    text = text.replace(
        'target_link_directories\t(${PROJECT_NAME} PUBLIC "${PROJECT_ROOT}/lib")',
        'target_link_directories(${PROJECT_NAME} PUBLIC "${PS5_PAYLOAD_SDK}/target/lib")',
    )
    text = text.replace(
        'target_link_libraries\t(${PROJECT_NAME} PUBLIC kernel SceGnmDriver)',
        'target_link_libraries(${PROJECT_NAME} PUBLIC kernel_sys SceGnmDriver)',
    )
    text = text.replace(
        'target_link_libraries(${PROJECT_NAME} PUBLIC kernel SceGnmDriver)',
        'target_link_libraries(${PROJECT_NAME} PUBLIC kernel_sys SceGnmDriver)',
    )

    # Soften flags that break third-party / large enums
    text = text.replace("-Werror", "")
    text = text.replace("-pedantic -pedantic-errors", "")

    # Define V_FW if missing
    if "set(V_FW" not in text and "V_FW" in text:
        text = text.replace(
            "project(${basename} C CXX ASM)",
            'project(${basename} C CXX ASM)\nset(V_FW "0x80000000" CACHE STRING "FW version")',
        )

    # Soften strip post-build so a missing objcopy doesn't fail the whole target
    text = text.replace(
        "COMMAND ${CMAKE_OBJCOPY}",
        "COMMAND ${CMAKE_OBJCOPY} || true\n    # original:",
    )

    p.write_text(text)
    print(f"patched {p}")

if __name__ == "__main__":
    main()
