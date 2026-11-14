# SPDX-License-Identifier: MPL-2.0
include_guard(GLOBAL)
# Use the build-host interpreter, not Conan's target CPython config package.
# The latter may enforce its embedding-library version even for Interpreter.
find_package(Python3 QUIET MODULE COMPONENTS Interpreter)

# The app and the focused Qt-only tests share the translation resources and
# implementation, without pulling the shell's SDK/rendering dependencies.
function(pj_add_app_language)
    if(TARGET pj_app_language)
        return()
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Core LinguistTools)
    get_filename_component(app_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(ts_files "${app_dir}/translations/plotjuggler4_zh_CN.ts"
        "${app_dir}/translations/plotjuggler4_zh_TW.ts")
    set(qm_dir "${CMAKE_CURRENT_BINARY_DIR}/translations")

    add_library(pj_app_language STATIC
        "${app_dir}/src/AppLanguage.cpp"
        "${app_dir}/src/AppLanguage.h")
    target_include_directories(pj_app_language PUBLIC "${app_dir}/src")
    target_link_libraries(pj_app_language PUBLIC Qt6::Core)
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(pj_app_language PRIVATE ${PJ_WARNING_FLAGS})
    endif()

    # The full Qt widget vocabulary is required, rather than silently shipping
    # an English-only file dialog when a minimal Qt kit omits translations.
    set(qt_translations_dir "${QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR}")
    if(NOT qt_translations_dir)
        if(IS_ABSOLUTE "${QT6_INSTALL_TRANSLATIONS}")
            set(qt_translations_dir "${QT6_INSTALL_TRANSLATIONS}")
        else()
            set(qt_translations_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}")
        endif()
    endif()
    # Qt's merge helper otherwise prefixes even an absolute install directory.
    # Keep the override local to this function, including split-prefix Qt kits.
    set(QT_INTERNAL_ABSOLUTE_TRANSLATIONS_DIR "${qt_translations_dir}")
    foreach(language IN ITEMS zh_CN zh_TW)
        if(NOT EXISTS "${qt_translations_dir}/qtbase_${language}.qm")
            message(FATAL_ERROR
                "Chinese UI requires qtbase_${language}.qm in ${qt_translations_dir}. "
                "Install the Qt translations component matching this Qt kit.")
        endif()
    endforeach()

    # Qt6.9+ can merge Qt's own strings into the embedded catalog. There is no
    # runtime dependency on a developer machine's Qt translations directory.
    qt_add_lrelease(
        TS_FILES ${ts_files}
        LRELEASE_TARGET pj_app_lrelease
        QM_OUTPUT_DIRECTORY "${qm_dir}"
        QM_FILES_OUTPUT_VARIABLE qm_files
        MERGE_QT_TRANSLATIONS
        QT_TRANSLATION_CATALOGS qtbase
        OPTIONS -nounfinished)
    qt_add_resources(pj_app_language pj_app_translations
        PREFIX "/i18n"
        BASE "${qm_dir}"
        FILES ${qm_files})
    add_dependencies(pj_app_language pj_app_lrelease)

    # Refresh only on explicit request, using the same extraction scope as the
    # coverage check. Ordinary builds must never rewrite the tracked catalog.
    if(Python3_Interpreter_FOUND)
        add_custom_target(pj_app_lupdate
            COMMAND "${Python3_EXECUTABLE}" "${app_dir}/../tests/check_translations_4x.py"
                --lupdate $<TARGET_FILE:Qt6::lupdate> --update-ts
            COMMAND "${Python3_EXECUTABLE}" "${app_dir}/../tests/check_translations_4x.py"
                --lupdate $<TARGET_FILE:Qt6::lupdate> --update-ts
                --catalog "${app_dir}/translations/plotjuggler4_zh_TW.ts"
            WORKING_DIRECTORY "${app_dir}/.."
            VERBATIM)
    endif()
endfunction()

# These tests run against the production catalog (no test-only translations).
function(pj_add_app_language_test)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    get_filename_component(app_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    add_executable(app_language_test "${app_dir}/tests/app_language_test.cpp")
    set_target_properties(app_language_test PROPERTIES AUTOMOC ON)
    target_link_libraries(app_language_test PRIVATE pj_app_language Qt6::Test)
    # Catalog assertions contain Chinese literals; do not use the host codepage.
    if(MSVC)
        target_compile_options(app_language_test PRIVATE /utf-8)
    endif()
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(app_language_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    get_filename_component(qt_translations_absolute
        "${QT6_INSTALL_TRANSLATIONS}" ABSOLUTE BASE_DIR "${QT6_INSTALL_PREFIX}")
    file(RELATIVE_PATH qt_translations_relative "${QT6_INSTALL_PREFIX}" "${qt_translations_absolute}")
    add_test(NAME QtTranslationPaths
        COMMAND "${CMAKE_COMMAND}"
            "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/translation-path-tests"
            "-DQT_CONFIG_DIR=${Qt6_DIR}"
            "-DQT_TRANSLATIONS_ABSOLUTE=${qt_translations_absolute}"
            "-DQT_TRANSLATIONS_RELATIVE=${qt_translations_relative}"
            "-DLANGUAGE_CMAKE=${app_dir}/cmake/PjAppLanguage.cmake"
            -P "${app_dir}/tests/i18n/check_translation_paths.cmake")
    add_test(NAME AppLanguageTest COMMAND app_language_test)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_executable(qt_file_dialog_language_test "${app_dir}/tests/qt_file_dialog_language_test.cpp")
    set_target_properties(qt_file_dialog_language_test PROPERTIES AUTOMOC ON)
    target_link_libraries(qt_file_dialog_language_test PRIVATE pj_app_language Qt6::Widgets Qt6::Test)
    if(MSVC)
        target_compile_options(qt_file_dialog_language_test PRIVATE /utf-8)
    endif()
    if(DEFINED PJ_WARNING_FLAGS)
        target_compile_options(qt_file_dialog_language_test PRIVATE ${PJ_WARNING_FLAGS})
    endif()
    add_test(NAME QtFileDialogLanguageTest COMMAND qt_file_dialog_language_test)
    set_tests_properties(QtFileDialogLanguageTest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(Python3_Interpreter_FOUND)
        add_test(NAME TranslationCatalog4x
            COMMAND "${Python3_EXECUTABLE}" "${app_dir}/../tests/check_translations_4x.py"
                --lupdate $<TARGET_FILE:Qt6::lupdate>)
        add_test(NAME TraditionalTranslationCatalog4x
            COMMAND "${Python3_EXECUTABLE}" "${app_dir}/../tests/check_translations_4x.py"
                --lupdate $<TARGET_FILE:Qt6::lupdate>
                --catalog "${app_dir}/translations/plotjuggler4_zh_TW.ts")
        add_test(NAME TranslationValidator4x
            COMMAND "${Python3_EXECUTABLE}" "${app_dir}/../tests/test_check_translations_4x.py")
        set_tests_properties(TranslationCatalog4x TraditionalTranslationCatalog4x TranslationValidator4x PROPERTIES
            WORKING_DIRECTORY "${app_dir}/.."
            TIMEOUT 120)
        # A catalog that only lags the sources (strings shown in English) is a
        # warning, never a failure: a UI change must not need a translation.
        set_tests_properties(TranslationCatalog4x TraditionalTranslationCatalog4x PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endfunction()
