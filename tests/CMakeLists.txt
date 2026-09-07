add_executable(CubicSDRTests
    TestMain.cpp
    AnalogModemTests.cpp
    AudioMixerTests.cpp
    DataTreeTests.cpp
    IOAndProcessingTests.cpp
    ModemIQTests.cpp
    SampleFrameAccumulatorTests.cpp
    SDRDataTests.cpp
    SpectrumVisualProcessorTests.cpp
    ThreadBlockingQueueTests.cpp
    UtilityTests.cpp
    ${PROJECT_SOURCE_DIR}/src/IOThread.cpp
    ${PROJECT_SOURCE_DIR}/src/audio/AudioMixer.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/Modem.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/ModemAnalog.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemAM.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemCW.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemDSB.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemFM.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemFMStereo.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemIQ.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemLSB.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemNBFM.cpp
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog/ModemUSB.cpp
    ${PROJECT_SOURCE_DIR}/src/process/FFTDataDistributor.cpp
    ${PROJECT_SOURCE_DIR}/src/process/SpectrumVisualProcessor.cpp
    ${PROJECT_SOURCE_DIR}/src/sdr/SDRDeviceInfo.cpp
    ${PROJECT_SOURCE_DIR}/src/util/DataTree.cpp
    ${PROJECT_SOURCE_DIR}/src/util/Gradient.cpp
    ${PROJECT_SOURCE_DIR}/src/util/Timer.cpp
    ${PROJECT_SOURCE_DIR}/external/tinyxml/tinyxml.cpp
    ${PROJECT_SOURCE_DIR}/external/tinyxml/tinystr.cpp
    ${PROJECT_SOURCE_DIR}/external/tinyxml/tinyxmlparser.cpp
    ${PROJECT_SOURCE_DIR}/external/tinyxml/tinyxmlerror.cpp
)

if(ENABLE_DIGITAL_LAB)
    target_sources(CubicSDRTests PRIVATE
        DigitalModemTests.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/ModemDigital.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemAPSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemASK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemBPSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemDPSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemFSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemGMSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemOOK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemPSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemQAM.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemQPSK.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemSQAM.cpp
        ${PROJECT_SOURCE_DIR}/src/modules/modem/digital/ModemST.cpp
    )
endif()

target_include_directories(CubicSDRTests PRIVATE
    ${LIQUID_INCLUDES}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/audio
    ${PROJECT_SOURCE_DIR}/src/demod
    ${PROJECT_SOURCE_DIR}/src/dsp
    ${PROJECT_SOURCE_DIR}/src/modules/modem
    ${PROJECT_SOURCE_DIR}/src/modules/modem/analog
    ${PROJECT_SOURCE_DIR}/src/modules/modem/digital
    ${PROJECT_SOURCE_DIR}/src/process
    ${PROJECT_SOURCE_DIR}/src/util
    ${PROJECT_SOURCE_DIR}/external/rnnoise/include
    ${PROJECT_SOURCE_DIR}/external/rtaudio
    ${PROJECT_SOURCE_DIR}/external/tinyxml
)

target_link_libraries(CubicSDRTests PRIVATE ${LIQUID_LIBRARIES} ${SOAPY_SDR_LIBRARY})

add_test(NAME CubicSDR.UnitTests COMMAND CubicSDRTests)
set_tests_properties(CubicSDR.UnitTests PROPERTIES TIMEOUT 15)

option(CUBICSDR_TEST_COVERAGE "Instrument CubicSDRTests and create a coverage report target" OFF)
if(CUBICSDR_TEST_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(CubicSDRTests PRIVATE -fprofile-instr-generate -fcoverage-mapping)
    set_property(TARGET CubicSDRTests APPEND_STRING PROPERTY LINK_FLAGS " -fprofile-instr-generate -fcoverage-mapping")

    if(APPLE)
        execute_process(COMMAND xcrun --find llvm-profdata
            OUTPUT_VARIABLE LLVM_PROFDATA_EXECUTABLE OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND xcrun --find llvm-cov
            OUTPUT_VARIABLE LLVM_COV_EXECUTABLE OUTPUT_STRIP_TRAILING_WHITESPACE)
    else()
        find_program(LLVM_PROFDATA_EXECUTABLE llvm-profdata REQUIRED)
        find_program(LLVM_COV_EXECUTABLE llvm-cov REQUIRED)
    endif()

    set(COVERAGE_RAW_FILE "${CMAKE_CURRENT_BINARY_DIR}/CubicSDRTests.profraw")
    set(COVERAGE_DATA_FILE "${CMAKE_CURRENT_BINARY_DIR}/CubicSDRTests.profdata")
    set(COVERAGE_HTML_DIR "${CMAKE_CURRENT_BINARY_DIR}/coverage")

    add_custom_target(CubicSDRCoverage
        COMMAND ${CMAKE_COMMAND} -E rm -f "${COVERAGE_RAW_FILE}" "${COVERAGE_DATA_FILE}"
        COMMAND ${CMAKE_COMMAND} -E env "LLVM_PROFILE_FILE=${COVERAGE_RAW_FILE}" $<TARGET_FILE:CubicSDRTests>
        COMMAND "${LLVM_PROFDATA_EXECUTABLE}" merge -sparse "${COVERAGE_RAW_FILE}" -o "${COVERAGE_DATA_FILE}"
        COMMAND ${CMAKE_COMMAND}
            "-DLLVM_COV_EXECUTABLE=${LLVM_COV_EXECUTABLE}"
            "-DTEST_BINARY=$<TARGET_FILE:CubicSDRTests>"
            "-DCOVERAGE_DATA_FILE=${COVERAGE_DATA_FILE}"
            -DCOVERAGE_MIN_PERCENT=80
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCoverage.cmake"
        COMMAND "${LLVM_COV_EXECUTABLE}" show $<TARGET_FILE:CubicSDRTests>
            "--instr-profile=${COVERAGE_DATA_FILE}"
            "--ignore-filename-regex=(/tests/|/external/)"
            -format=html "--output-dir=${COVERAGE_HTML_DIR}"
        DEPENDS CubicSDRTests
        USES_TERMINAL
        VERBATIM
    )
endif()
