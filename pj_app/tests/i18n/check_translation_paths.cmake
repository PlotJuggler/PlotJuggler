# SPDX-License-Identifier: MPL-2.0
# Configure the production helper against the real Qt kit, varying only the
# exported translation-directory layout. No installed Qt files are modified.
foreach(mode IN ITEMS relative absolute missing)
    set(source_dir "${TEST_BINARY_DIR}/${mode}/src")
    set(binary_dir "${TEST_BINARY_DIR}/${mode}/build")
    file(MAKE_DIRECTORY "${source_dir}/modules")
    if(mode STREQUAL "relative")
        set(layout "set(QT6_INSTALL_TRANSLATIONS \"${QT_TRANSLATIONS_RELATIVE}\")")
    elseif(mode STREQUAL "absolute")
        set(layout "set(QT6_INSTALL_TRANSLATIONS \"${QT_TRANSLATIONS_ABSOLUTE}\")")
    else()
        set(layout "set(QT6_INSTALL_TRANSLATIONS \"${binary_dir}/missing-translations\")")
    endif()
    file(WRITE "${source_dir}/modules/FindQt6.cmake"
        "include(\"${QT_CONFIG_DIR}/Qt6Config.cmake\")\n"
        "unset(QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR)\n${layout}\n")
    file(WRITE "${source_dir}/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.22)\nproject(translation_path_test LANGUAGES CXX)\n"
        "list(PREPEND CMAKE_MODULE_PATH \"${source_dir}/modules\")\n"
        "include(\"${LANGUAGE_CMAKE}\")\npj_add_app_language()\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${binary_dir}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(mode STREQUAL "missing")
        if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "requires qtbase_zh_CN.qm")
            message(FATAL_ERROR "Missing-catalog regression: ${result}\n${output}${error}")
        endif()
    else()
        if(NOT result EQUAL 0 OR "${output}${error}" MATCHES "cannot be found for language")
            message(FATAL_ERROR "${mode} translation path failed: ${output}${error}")
        endif()
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${binary_dir}" --target pj_app_lrelease
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(NOT result EQUAL 0 OR "${output}${error}" MATCHES "dropping duplicate messages")
            message(FATAL_ERROR "${mode} catalog merge failed: ${output}${error}")
        endif()
    endif()
endforeach()
