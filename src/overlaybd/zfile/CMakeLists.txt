file(GLOB SOURCE_ZFILE "*.cpp")
file(GLOB SOURCE_LZ4 "lz4/*.c" "lz4/*.cpp")
file(GLOB SOURCE_CRC32 "crc32/crc32c.cpp")

set (CMAKE_CXX_STANDARD 17)
add_library(crc32_lib STATIC ${SOURCE_CRC32})
target_include_directories(crc32_lib PUBLIC
    ${PHOTON_INCLUDE_DIR}
)

if (CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64)
    target_compile_options(crc32_lib PUBLIC -msse4.2 -mcrc32)
else()
    if (NOT CMAKE_CXX_FLAGS MATCHES "-march=|-mcpu=")
        check_cxx_compiler_flag(-mcpu=native COMPILER_HAS_NATIVE_FLAG)
        if (COMPILER_HAS_NATIVE_FLAG)
            target_compile_options(crc32_lib PRIVATE -mcpu=native)
        else ()
            target_compile_options(crc32_lib PRIVATE -mcpu=generic+crc)
        endif ()
    endif ()
endif()

if(ENABLE_DSA OR ENABLE_ISAL)
    add_subdirectory(thirdparty)
    add_dependencies(crc32_lib thirdparty_lib)
    target_link_directories(crc32_lib PUBLIC ${LIBRARY_OUTPUT_PATH})
    target_include_directories(crc32_lib PUBLIC ${LIBRARY_OUTPUT_PATH}/include)
    if(ENABLE_DSA)
        target_link_libraries(crc32_lib -lpci -ldmlhl -ldl)
        target_compile_definitions(crc32_lib PUBLIC -DENABLE_DSA)
    endif()
    if(ENABLE_ISAL)
        target_compile_options(crc32_lib PUBLIC -mavx512f)
        target_compile_definitions(crc32_lib PUBLIC -DENABLE_ISAL)
        target_link_libraries(crc32_lib -lisal)
    endif()
endif()
set (CMAKE_CXX_STANDARD 14)

# ---- QAT auto-detection ----
if (ENABLE_QAT)
    find_path(QAT_INCLUDE_DIR NAMES qat/cpa.h
              PATHS /usr/include/qat /usr/local/include/qat)
    find_library(QAT_LIBRARY NAMES qat)
    find_library(USDM_LIBRARY NAMES usdm)
    if (QAT_INCLUDE_DIR AND QAT_LIBRARY AND USDM_LIBRARY)
        message(STATUS "QAT acceleration: ENABLED (include=${QAT_INCLUDE_DIR})")
    else()
        message(WARNING "ENABLE_QAT=ON but QAT headers/libs not found; disabling QAT")
        set(ENABLE_QAT OFF)
    endif()
endif()
if (NOT ENABLE_QAT)
    # When QAT is off, exclude lz4-qat.cpp so we don't need QAT headers at all
    list(REMOVE_ITEM SOURCE_LZ4 "${CMAKE_CURRENT_SOURCE_DIR}/lz4/lz4-qat.cpp")
    message(STATUS "QAT acceleration: DISABLED")
endif()
# --------------------------------

# ---- KAE (Kunpeng Accelerator Engine) ----
# KAE libraries are dlopen()ed at runtime and never linked (their ELF
# constructors abort() on unsupported CPUs, so loading is gated at runtime by
# kae_support.cpp). No KAE headers or libs are needed at build time, so
# ENABLE_KAE=ON compiles everywhere and simply stays dormant off-platform.
if (ENABLE_KAE)
    # The kae/ directory is not covered by the top-level GLOB, so the loader
    # sources are added explicitly here.
    list(APPEND SOURCE_ZFILE "${CMAKE_CURRENT_SOURCE_DIR}/kae/kae_support.cpp")
    list(APPEND SOURCE_ZFILE "${CMAKE_CURRENT_SOURCE_DIR}/kae/kae_zstd.cpp")
    message(STATUS "KAE acceleration: ENABLED (runtime-gated, dlopen)")
else()
    # When KAE is off, exclude the KAE sources entirely.
    list(REMOVE_ITEM SOURCE_LZ4 "${CMAKE_CURRENT_SOURCE_DIR}/lz4/lz4-kae.cpp")
    list(REMOVE_ITEM SOURCE_ZFILE "${CMAKE_CURRENT_SOURCE_DIR}/kae/kae_support.cpp")
    list(REMOVE_ITEM SOURCE_ZFILE "${CMAKE_CURRENT_SOURCE_DIR}/kae/kae_zstd.cpp")
    message(STATUS "KAE acceleration: DISABLED")
endif()
# --------------------------------

add_library(zfile_lib STATIC ${SOURCE_ZFILE} ${SOURCE_LZ4})
target_link_libraries(zfile_lib photon_static crc32_lib ${LIBZSTD})

if (ENABLE_QAT)
    target_compile_definitions(zfile_lib PUBLIC -DENABLE_QAT)
    target_include_directories(zfile_lib PUBLIC ${QAT_INCLUDE_DIR})
    target_link_libraries(zfile_lib ${QAT_LIBRARY} ${USDM_LIBRARY} -lpci -lpthread)
endif()

if (ENABLE_KAE)
    target_compile_definitions(zfile_lib PUBLIC -DENABLE_KAE)
    # kae_support.cpp uses dlopen/dlsym.
    target_link_libraries(zfile_lib ${CMAKE_DL_LIBS})
endif()

if (BUILD_TESTING)
    add_subdirectory(test)
endif ()
