cmake_minimum_required(VERSION 3.16)

# melonDS needs its build-flag overrides in place before project().
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/melonDS/cmake/DefaultBuildFlags.cmake")

project(NativeDSPort LANGUAGES C CXX)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if (NOT WIN32 OR NOT MINGW)
    message(FATAL_ERROR "This port targets Windows with the MSYS2 MinGW-w64 toolchain.")
endif()

set(GAME_NAME "Pokemon - Black (BR)" CACHE STRING "Name of the final executable and window title")
set(GAME_ROM "${CMAKE_CURRENT_SOURCE_DIR}/${GAME_NAME}.nds" CACHE FILEPATH "Source .nds image to embed")
set(GAME_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Where the final .exe is written")

if (NOT EXISTS "${GAME_ROM}")
    message(FATAL_ERROR "ROM not found: ${GAME_ROM} (set -DGAME_ROM=...)")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(SDL2 REQUIRED CONFIG)

# --- melonDS core (static library, no Qt frontend) ---------------------------
set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
set(ENABLE_JIT ON CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/melonDS)

# --- Asset pipeline: ROM -> trimmed payload + icon + resource script ----------
set(ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")
set(PACK_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_rom.py")
set(PACK_OUTPUTS "${ASSET_DIR}/game.bin" "${ASSET_DIR}/game.ico" "${ASSET_DIR}/game.rc" "${ASSET_DIR}/rom_info.h")

add_custom_command(
    OUTPUT ${PACK_OUTPUTS}
    COMMAND "${Python3_EXECUTABLE}" -I "${PACK_SCRIPT}" pack --rom "${GAME_ROM}" --out "${ASSET_DIR}" --name "${GAME_NAME}"
    DEPENDS "${GAME_ROM}" "${PACK_SCRIPT}"
    COMMENT "Packing ROM assets for embedding"
    VERBATIM)
add_custom_target(pack_assets DEPENDS ${PACK_OUTPUTS})

# Full dump (code, overlays, NitroFS) for the static-recompilation work; not embedded.
add_custom_target(extract_assets
    COMMAND "${Python3_EXECUTABLE}" -I "${PACK_SCRIPT}" extract --rom "${GAME_ROM}" --out "${CMAKE_CURRENT_BINARY_DIR}/extracted"
    DEPENDS "${PACK_SCRIPT}"
    COMMENT "Extracting ARM9/ARM7 code, overlays and NitroFS"
    VERBATIM)

set_source_files_properties("${ASSET_DIR}/game.rc" PROPERTIES
    GENERATED TRUE
    OBJECT_DEPENDS "${ASSET_DIR}/game.bin;${ASSET_DIR}/game.ico")

# --- Game executable -----------------------------------------------------------
add_executable(game WIN32
    src/main.cpp
    src/Platform.cpp
    src/App.h
    src/Input.cpp
    src/Input.h
    src/ControlsMenu.cpp
    src/ControlsMenu.h
    src/TextRenderer.cpp
    src/TextRenderer.h
    "${ASSET_DIR}/game.rc")
add_dependencies(game pack_assets)

set_target_properties(game PROPERTIES
    OUTPUT_NAME "${GAME_NAME}"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${GAME_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GAME_OUTPUT_DIR}")

target_include_directories(game PRIVATE "${ASSET_DIR}")
target_compile_definitions(game PRIVATE
    "GAME_DISPLAY_NAME=\"${GAME_NAME}\""
    ARCHITECTURE_x86_64=1
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)

# Everything static: the .exe must run on a clean Windows install with no DLLs beside it.
target_link_libraries(game PRIVATE core SDL2::SDL2-static)
target_link_options(game PRIVATE -static -static-libgcc -static-libstdc++)

# --- Static recompilation (work in progress) ------------------------------------------
# Extracts the ARM9 code, recompiles it to C and builds a differential test that
# checks every recompiled function against melonDS's interpreter.
option(ENABLE_RECOMP "Generate the ARM9 static recompilation and its differential test" OFF)
if (ENABLE_RECOMP)
    set(EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/extracted")
    set(RECOMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/recomp")
    # Runs at configure time: the list of generated sources must be known up front.
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I "${PACK_SCRIPT}" extract --rom "${GAME_ROM}" --out "${EXTRACT_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    option(RECOMP_STATIC_ONLY "Recompile only the static ARM9 module (skip the 237 overlays; much faster builds)" OFF)
    set(RECOMP_ARGS --layout "${EXTRACT_DIR}/layout.json" --out "${RECOMP_DIR}")
    if (RECOMP_STATIC_ONLY)
        list(APPEND RECOMP_ARGS --only-static)
    endif()
    # Entry points observed at run time that static analysis can't find.
    set(RECOMP_SEEDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/seeds_IRBO.txt")
    if (EXISTS "${RECOMP_SEEDS}")
        list(APPEND RECOMP_ARGS --seeds "${RECOMP_SEEDS}")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/recomp.py" ${RECOMP_ARGS}
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/recomp.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/armdecode.py"
        "${RECOMP_SEEDS}"
        "${PACK_SCRIPT}")

    file(STRINGS "${RECOMP_DIR}/sources.txt" RECOMP_SOURCES)
    list(TRANSFORM RECOMP_SOURCES PREPEND "${RECOMP_DIR}/")

    add_library(recomp_arm9 STATIC ${RECOMP_SOURCES})
    target_include_directories(recomp_arm9 PUBLIC src/recomp "${RECOMP_DIR}")
    # Huge generated functions: -O1 keeps compile times sane; -fwrapv matches ARM arithmetic.
    target_compile_options(recomp_arm9 PRIVATE -O1 -fwrapv -w)
    target_compile_definitions(recomp_arm9 PRIVATE RECOMP_HOOK_BLOCKS RECOMP_CHECK_RETURNS)

    add_executable(recomp_difftest src/recomp/difftest.cpp src/Platform.cpp)
    set_target_properties(recomp_difftest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_include_directories(recomp_difftest PRIVATE src)
    target_compile_definitions(recomp_difftest PRIVATE ARCHITECTURE_x86_64=1 WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(recomp_difftest PRIVATE recomp_arm9 core SDL2::SDL2-static)
    # 64 MB stack: recompiled call chains nest as deeply as the game's own.
    target_link_options(recomp_difftest PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--stack,67108864)

    # --- Hybrid mode: melonDS emulates the console, whitelisted ARM9 functions run recompiled ---
    option(ENABLE_HYBRID "Build game_hybrid (--hybrid flag) and recomp_hybridtest" OFF)
    if (ENABLE_HYBRID)
        # The core needs two hooks in ARMv5 (function entry detection). The submodule is
        # left untouched: a patched copy of ARM.cpp replaces the original in `core`.
        set(MELON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/third_party/melonDS/src")
        set(HYBRID_DIR "${CMAKE_CURRENT_BINARY_DIR}/hybrid")
        set(HYBRID_PATCH "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/patch_core_hybrid.py")
        file(MAKE_DIRECTORY "${HYBRID_DIR}")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -I "${HYBRID_PATCH}" "${MELON_SRC}/ARM.cpp" "${HYBRID_DIR}/ARM.cpp"
            COMMAND_ERROR_IS_FATAL ANY)
        get_target_property(CORE_SOURCES core SOURCES)
        list(TRANSFORM CORE_SOURCES REPLACE "^ARM\\.cpp$" "${HYBRID_DIR}/ARM.cpp")
        set_property(TARGET core PROPERTY SOURCES ${CORE_SOURCES})
        target_include_directories(core PRIVATE "${MELON_SRC}")

        # Functions the differential test proved identical (difftest --passlist).
        set(HYBRID_WHITELIST "${CMAKE_CURRENT_SOURCE_DIR}/tools/recomp/hybrid_whitelist_IRBO.txt")
        file(STRINGS "${HYBRID_WHITELIST}" WL_LINES REGEX "^[0-9A-Fa-f]+")
        set(WL_C "#include <stdint.h>\nconst uint32_t hybrid_whitelist[] = {\n")
        foreach (addr IN LISTS WL_LINES)
            string(APPEND WL_C "0x${addr}u,\n")
        endforeach()
        list(LENGTH WL_LINES WL_COUNT)
        string(APPEND WL_C "0};\nconst unsigned hybrid_whitelist_size = ${WL_COUNT};\n")
        file(WRITE "${HYBRID_DIR}/hybrid_whitelist.c" "${WL_C}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${HYBRID_PATCH}" "${HYBRID_WHITELIST}" "${MELON_SRC}/ARM.cpp")

        add_library(recomp_hybrid STATIC src/recomp/hybrid.cpp "${HYBRID_DIR}/hybrid_whitelist.c")
        set_target_properties(recomp_hybrid PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        target_compile_definitions(recomp_hybrid PRIVATE ARCHITECTURE_x86_64=1)
        target_include_directories(recomp_hybrid PUBLIC src/recomp)
        target_link_libraries(recomp_hybrid PUBLIC recomp_arm9 core)

        # Same game, plus the --hybrid flag. Separate .exe so the regular one stays as it is.
        get_target_property(GAME_SOURCES game SOURCES)
        add_executable(game_hybrid WIN32 ${GAME_SOURCES})
        add_dependencies(game_hybrid pack_assets)
        set_target_properties(game_hybrid PROPERTIES
            OUTPUT_NAME "${GAME_NAME} (hybrid)"
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            RUNTIME_OUTPUT_DIRECTORY "${GAME_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GAME_OUTPUT_DIR}")
        target_include_directories(game_hybrid PRIVATE "${ASSET_DIR}")
        get_target_property(GAME_DEFS game COMPILE_DEFINITIONS)
        target_compile_definitions(game_hybrid PRIVATE ${GAME_DEFS} HYBRID_RECOMP=1)
        target_link_libraries(game_hybrid PRIVATE recomp_hybrid core SDL2::SDL2-static)
        target_link_options(game_hybrid PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--stack,67108864)

        # Headless: boots the .nds with or without --hybrid, presses buttons on a script,
        # saves screenshots and prints the hybrid counters.
        add_executable(recomp_hybridtest src/recomp/hybridtest.cpp src/Platform.cpp)
        set_target_properties(recomp_hybridtest PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        target_include_directories(recomp_hybridtest PRIVATE src)
        target_compile_definitions(recomp_hybridtest PRIVATE ARCHITECTURE_x86_64=1 WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(recomp_hybridtest PRIVATE recomp_hybrid core SDL2::SDL2-static)
        target_link_options(recomp_hybridtest PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--stack,67108864)
    endif()
elseif (ENABLE_HYBRID)
    message(FATAL_ERROR "ENABLE_HYBRID needs ENABLE_RECOMP=ON")
endif()
