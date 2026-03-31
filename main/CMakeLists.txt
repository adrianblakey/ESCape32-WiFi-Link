idf_component_register(SRCS main.c PRIV_REQUIRES espressif__mdns)
set(lang_map en:EN fr:FR de:DE it:IT ru:РУ zh:中)
string(REGEX REPLACE "([a-z][a-z]):([^;]+)" "XX(\\1)" LANG_LIST "${lang_map}")
string(REGEX REPLACE "([a-z][a-z]):([^;]+)" "<option value=\"\\1\">\\2</option>" LANG_OPTS "${lang_map}")
list(JOIN LANG_LIST " " LANG_LIST)
list(JOIN LANG_OPTS "" LANG_OPTS)

# Build inline JSON object for all language files: {"en":{...},"de":{...},...}
# This is substituted into @LANG_DATA@ in root.html so setlang() never needs
# a network fetch (which would block on the single-threaded HTTP server).
set(LANG_DATA "{")
set(_first TRUE)
foreach(pair ${lang_map})
    string(REGEX MATCH "^([a-z][a-z]):" _ "${pair}")
    set(lang "${CMAKE_MATCH_1}")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/root_${lang}.json")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/root_${lang}.json" _json_raw)
        string(STRIP "${_json_raw}" _json_raw)
        if(NOT _first)
            string(APPEND LANG_DATA ",")
        endif()
        string(APPEND LANG_DATA "\"${lang}\":${_json_raw}")
        set(_first FALSE)
    endif()
endforeach()
string(APPEND LANG_DATA "}")

configure_file(build_defs.h.in build_defs.h @ONLY)
target_include_directories(${COMPONENT_TARGET} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
file(GLOB files root*.*)
foreach(file ${files})
	get_filename_component(name ${file} NAME)
	configure_file(${name} ${name} @ONLY)
	add_custom_command(
		OUTPUT ${name}.gz
		COMMAND gzip -f ${name}
		DEPENDS ${name}
		WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	)
	target_add_binary_data(${COMPONENT_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz BINARY DEPENDS ${name}.gz)
endforeach()
