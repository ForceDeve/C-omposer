find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# Templates live as real files in templates/ and are embedded at build time.
set(TEMPLATE_NAMES basic test_functions)
set(TEMPLATES_C ${CMAKE_CURRENT_BINARY_DIR}/templates_data.c)
set(TEMPLATE_FILES)
foreach(name ${TEMPLATE_NAMES})
    list(APPEND TEMPLATE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/templates/${name}.cmp)
endforeach()
string(REPLACE ";" "\;" TEMPLATE_NAMES_ARG "${TEMPLATE_NAMES}")
add_custom_command(
        OUTPUT ${TEMPLATES_C}
        COMMAND ${CMAKE_COMMAND} -DOUT=${TEMPLATES_C} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/templates
                "-DNAMES=${TEMPLATE_NAMES_ARG}" -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_templates.cmake
        DEPENDS ${TEMPLATE_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/embed_templates.cmake
        COMMENT "Embedding editor templates")

add_executable(C_omposer
        src/main.c
        src/editor.c
        src/completion.c
        src/symbols.c
        src/lexer.c
        src/language.c
        src/theme.c
        src/runner.c
        src/settings.c
        src/sighelp.c
        src/sidebar.c
        src/visuals.c
        src/prefs.c
        ${TEMPLATES_C}
)
target_link_libraries(C_omposer PRIVATE PkgConfig::GTK4 lang_music)
target_compile_options(C_omposer PRIVATE -Wall -Wextra)
target_compile_definitions(C_omposer PRIVATE COMPOSER_THEME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/themes")
