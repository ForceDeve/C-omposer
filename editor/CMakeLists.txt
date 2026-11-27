find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

add_executable(C_omposer
        src/main.c
        src/editor.c
        src/completion.c
        src/symbols.c
        src/lexer.c
        src/language.c
        src/theme.c
)
target_link_libraries(C_omposer PRIVATE PkgConfig::GTK4)
target_compile_options(C_omposer PRIVATE -Wall -Wextra)
