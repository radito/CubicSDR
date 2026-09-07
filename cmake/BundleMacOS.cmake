cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED APP_BUNDLE OR NOT IS_DIRECTORY "${APP_BUNDLE}")
    message(FATAL_ERROR "APP_BUNDLE must identify an existing macOS app bundle")
endif()

if(NOT DEFINED DEPENDENCY_DIRS)
    set(DEPENDENCY_DIRS "")
endif()

file(GLOB SOAPY_PLUGINS "${APP_BUNDLE}/Contents/MacOS/modules/*.so")
if(NOT SOAPY_PLUGINS)
    message(FATAL_ERROR "No bundled SoapySDR modules were found")
endif()

# Some SoapyRTLSDR builds retain a versioned Homebrew install name which no
# longer exists after librtlsdr is upgraded (for example librtlsdr.2.dylib
# while Homebrew provides librtlsdr.0.dylib). BundleUtilities tries to inspect
# that missing file before it can copy anything, so repair the copied plugin
# first.
set(RTLSDR_PLUGIN "${APP_BUNDLE}/Contents/MacOS/modules/librtlsdrSupport.so")
if(EXISTS "${RTLSDR_PLUGIN}")
    find_program(OTOOL_EXECUTABLE otool REQUIRED)
    find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool REQUIRED)

    execute_process(
        COMMAND "${OTOOL_EXECUTABLE}" -L "${RTLSDR_PLUGIN}"
        RESULT_VARIABLE OTOOL_RESULT
        OUTPUT_VARIABLE RTLSDR_DEPENDENCIES
        ERROR_VARIABLE OTOOL_ERROR
    )
    if(NOT OTOOL_RESULT EQUAL 0)
        message(FATAL_ERROR "Unable to inspect ${RTLSDR_PLUGIN}: ${OTOOL_ERROR}")
    endif()

    string(REGEX MATCH "/[^ \n]+/librtlsdr\.[0-9]+\.dylib" RTLSDR_OLD_LIBRARY "${RTLSDR_DEPENDENCIES}")
    if(RTLSDR_OLD_LIBRARY AND NOT EXISTS "${RTLSDR_OLD_LIBRARY}")
        set(RTLSDR_REPLACEMENT "")
        get_filename_component(RTLSDR_OLD_DIRECTORY "${RTLSDR_OLD_LIBRARY}" DIRECTORY)
        set(RTLSDR_SEARCH_DIRS "${RTLSDR_OLD_DIRECTORY}" ${DEPENDENCY_DIRS})

        foreach(RTLSDR_SEARCH_DIR IN LISTS RTLSDR_SEARCH_DIRS)
            foreach(RTLSDR_NAME librtlsdr.0.dylib librtlsdr.dylib)
                if(EXISTS "${RTLSDR_SEARCH_DIR}/${RTLSDR_NAME}")
                    set(RTLSDR_REPLACEMENT "${RTLSDR_SEARCH_DIR}/${RTLSDR_NAME}")
                    break()
                endif()
            endforeach()
            if(RTLSDR_REPLACEMENT)
                break()
            endif()
        endforeach()

        if(NOT RTLSDR_REPLACEMENT)
            message(FATAL_ERROR
                "${RTLSDR_PLUGIN} references missing ${RTLSDR_OLD_LIBRARY}, and no replacement librtlsdr was found")
        endif()

        execute_process(
            COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}" -change
                    "${RTLSDR_OLD_LIBRARY}" "${RTLSDR_REPLACEMENT}" "${RTLSDR_PLUGIN}"
            RESULT_VARIABLE INSTALL_NAME_RESULT
            ERROR_VARIABLE INSTALL_NAME_ERROR
        )
        if(NOT INSTALL_NAME_RESULT EQUAL 0)
            message(FATAL_ERROR "Unable to repair ${RTLSDR_PLUGIN}: ${INSTALL_NAME_ERROR}")
        endif()
        message(STATUS "Repaired SoapyRTLSDR dependency: ${RTLSDR_OLD_LIBRARY} -> ${RTLSDR_REPLACEMENT}")
    endif()
endif()

include(BundleUtilities)
fixup_bundle("${APP_BUNDLE}" "${SOAPY_PLUGINS}" "${DEPENDENCY_DIRS}")
verify_app("${APP_BUNDLE}")
