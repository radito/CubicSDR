# Keep the existing report scope: production code linked into CubicSDRTests,
# excluding test sources and vendored code. This is not whole-app coverage.
execute_process(
    COMMAND "${LLVM_COV_EXECUTABLE}" report "${TEST_BINARY}"
        "--instr-profile=${COVERAGE_DATA_FILE}"
        "--ignore-filename-regex=(/tests/|/external/)"
    RESULT_VARIABLE report_status
    OUTPUT_VARIABLE report
    ERROR_VARIABLE report_error
)
if(NOT report_status EQUAL 0)
    message(FATAL_ERROR "Coverage report failed: ${report_error}")
endif()
# Reject stale/mismatched profiles instead of accepting misleading totals.
if(report_error MATCHES "out of date|mismatched")
    message(FATAL_ERROR "Coverage profile does not match the test binary: ${report_error}")
endif()
message("${report}")
string(REGEX MATCH "TOTAL[^\n\r]*" totals "${report}")
string(REGEX MATCHALL "[0-9]+\\.[0-9]+%" percentages "${totals}")
list(LENGTH percentages metric_count)
if(NOT metric_count EQUAL 4)
    message(FATAL_ERROR "Cannot read all four coverage metrics from: ${totals}")
endif()
set(metrics regions functions lines branches)
foreach(index RANGE 0 3)
    list(GET metrics ${index} metric)
    list(GET percentages ${index} percentage)
    string(REPLACE "%" "" percentage "${percentage}")
    if(percentage LESS COVERAGE_MIN_PERCENT)
        message(FATAL_ERROR "${metric} coverage ${percentage}% is below ${COVERAGE_MIN_PERCENT}%")
    endif()
endforeach()
message(STATUS "All four coverage metrics meet the ${COVERAGE_MIN_PERCENT}% minimum")
