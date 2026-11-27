# The language runtime: music types and their behaviour.
# The interpreter and the compiler/CLI will link this too.
add_library(lang_music STATIC src/music.c)
target_include_directories(lang_music PUBLIC include)
target_link_libraries(lang_music PUBLIC m)
target_compile_options(lang_music PRIVATE -Wall -Wextra)

add_executable(test_music tests/test_music.c)
target_link_libraries(test_music PRIVATE lang_music)
target_compile_options(test_music PRIVATE -Wall -Wextra)
add_test(NAME music COMMAND test_music)
