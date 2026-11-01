# SPDX-License-Identifier: MPL-2.0
# Runs plotjuggler4 headless on the Custom Widget layout and checks the exit code and the diagnostics.
#   -DAPP=<executable> -DLAYOUT=<layout> -DPLUGIN_DIR=<fixture dir> -DWORK_DIR=<scratch>
#   -DVARIANT=with_plugin|without_plugin|after_kill
# The layout names a placeholder (__DATA_FILE__) for the empty data file the mock file source reads.
# with_plugin:    exit 0, a "Loaded Widget" info record, the widget's own "first update" message
#                 (the widget was created, mounted and fed with its two wires), no error record.
# without_plugin: exit 0, a warning that the widget is missing, no error record.
# after_kill:     a first run with the widget live is killed (SIGKILL, through coreutils timeout,
#                 which reaps it: execute_process's own TIMEOUT leaves a zombie that still counts as
#                 alive) and must leave its active-widgets record (active_widgets.<pid>.json)
#                 naming it. A record is also planted under the pid of this cmake process, which is
#                 alive: it stands for a second instance still running. The second run is the
#                 with_plugin run plus a warning that the last run did not shut down cleanly; the
#                 dead run's record is consumed, the live one is neither reported nor erased.
# Every variant:  after the checked run, no active-widgets record of a dead run may remain (a clean
#                 exit removes the run's own; the planted live one is the only file allowed).

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/config" "${WORK_DIR}/extensions")
set(dump "${WORK_DIR}/diagnostics.json")
set(data_file "${WORK_DIR}/data.mock")
file(WRITE "${data_file}" "")
file(READ "${LAYOUT}" layout_text)
string(REPLACE "__DATA_FILE__" "${data_file}" layout_text "${layout_text}")
set(layout_file "${WORK_DIR}/layout.pj4.xml")
file(WRITE "${layout_file}" "${layout_text}")
# AppLocalDataLocation on Linux: $XDG_DATA_HOME/<organization>/<application>. One record per process.
set(records_dir "${WORK_DIR}/data/PlotJuggler/PlotJuggler4")
set(live_record "")

set(ENV{QT_QPA_PLATFORM} offscreen)
set(ENV{XDG_CONFIG_HOME} "${WORK_DIR}/config")
set(ENV{XDG_DATA_HOME} "${WORK_DIR}/data")
set(ENV{HOME} "${WORK_DIR}")

if(VARIANT STREQUAL "after_kill")
    # No --exit-after-layout: the run stays up with the widget live until the timeout kills it.
    # --foreground: only the application gets the SIGKILL, so timeout lives on to reap it.
    execute_process(
        COMMAND timeout --foreground -s KILL 15 ${APP} --layout "${layout_file}" --plugin-dir "${PLUGIN_DIR}"
        RESULT_VARIABLE killed_result
        OUTPUT_VARIABLE killed_out
        ERROR_VARIABLE killed_err)
    file(GLOB killed_records "${records_dir}/active_widgets.*.json")
    list(LENGTH killed_records killed_count)
    if(NOT killed_count EQUAL 1)
        message(FATAL_ERROR "the killed run (${killed_result}) left ${killed_count} active-widgets records in "
                            "${records_dir} (one expected)\n--- stdout\n${killed_out}\n--- stderr\n${killed_err}")
    endif()
    list(GET killed_records 0 dead_record)
    file(READ "${dead_record}" marker_text)
    if(NOT marker_text MATCHES "\"plugin\":\"pj_test_widgets\"")
        message(FATAL_ERROR "the active-widgets record does not name the widget:\n${marker_text}")
    endif()
    # The parent of a shell this script starts is this cmake process: alive for the whole check.
    execute_process(COMMAND sh -c "echo $PPID" OUTPUT_VARIABLE live_pid OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(live_record "${records_dir}/active_widgets.${live_pid}.json")
    file(WRITE "${live_record}" "{\"version\":1,\"widgets\":[{\"plugin\":\"running_widgets\",\"type\":\"gauge\"}]}")
endif()

set(args --layout "${layout_file}" --plugin-dir "${PLUGIN_DIR}" --exit-after-layout --exit-after-layout-timeout 60
    --dump-diagnostics "${dump}")

execute_process(
    COMMAND ${APP} ${args}
    RESULT_VARIABLE exit_code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 100)
if(NOT exit_code EQUAL 0)
    message(FATAL_ERROR "plotjuggler4 exited with ${exit_code}\n--- stdout\n${out}\n--- stderr\n${err}")
endif()
if(NOT EXISTS "${dump}")
    message(FATAL_ERROR "no diagnostics dump at ${dump}\n${err}")
endif()
file(GLOB remaining_records "${records_dir}/active_widgets.*.json")
if(NOT live_record STREQUAL "")
    if(NOT EXISTS "${live_record}")
        message(FATAL_ERROR "the review erased the record of a live process: ${live_record}")
    endif()
    list(REMOVE_ITEM remaining_records "${live_record}")
endif()
if(remaining_records)
    message(FATAL_ERROR "active-widgets records left behind after a clean exit: ${remaining_records}")
endif()

file(READ "${dump}" json)
string(JSON count LENGTH "${json}" records)
set(errors "")
set(saw_loaded FALSE)
set(saw_missing FALSE)
set(saw_value FALSE)
set(saw_unclean FALSE)
set(saw_live_record FALSE)
if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(JSON level GET "${json}" records ${i} level)
        string(JSON message GET "${json}" records ${i} message)
        string(JSON source GET "${json}" records ${i} source)
        if(level STREQUAL "error")
            string(APPEND errors "  ${source}: ${message}\n")
        endif()
        if(message MATCHES "^Loaded Widget ")
            set(saw_loaded TRUE)
        endif()
        if(source STREQUAL "CustomWidget" AND message MATCHES "^first update: mock/file_data/value, ")
            set(saw_value TRUE)
        endif()
        if(source STREQUAL "CustomWidget" AND level STREQUAL "warning" AND message MATCHES "is missing")
            set(saw_missing TRUE)
        endif()
        if(source STREQUAL "CustomWidget" AND level STREQUAL "warning" AND message MATCHES "did not shut down cleanly"
           AND message MATCHES "pj_test_widgets/label")
            set(saw_unclean TRUE)
        endif()
        if(message MATCHES "running_widgets")
            set(saw_live_record TRUE)
        endif()
    endforeach()
endif()
if(NOT errors STREQUAL "")
    message(FATAL_ERROR "error diagnostics:\n${errors}")
endif()
if(NOT VARIANT STREQUAL "without_plugin" AND NOT saw_loaded)
    message(FATAL_ERROR "the fixture widget library was not loaded\n${json}")
endif()
if(NOT VARIANT STREQUAL "without_plugin" AND NOT saw_value)
    message(FATAL_ERROR "the widget never received an update\n${json}")
endif()
if(VARIANT STREQUAL "without_plugin" AND NOT saw_missing)
    message(FATAL_ERROR "no warning about the missing widget\n${json}")
endif()
if(VARIANT STREQUAL "after_kill" AND NOT saw_unclean)
    message(FATAL_ERROR "no warning about the run that did not shut down cleanly\n${json}")
endif()
if(saw_live_record)
    message(FATAL_ERROR "the record of a process still alive was reported\n${json}")
endif()
if(NOT VARIANT STREQUAL "after_kill" AND saw_unclean)
    message(FATAL_ERROR "a fresh work directory reported an unclean shutdown\n${json}")
endif()
