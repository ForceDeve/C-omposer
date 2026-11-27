# The language runtime: music types, and the sound engine they drive.
# The interpreter and the compiler/CLI will link this too.
add_library(lang_music STATIC
        src/music.c
        src/audio_core.c
        src/synth.c
        src/sampler.c
        src/fx.c
)
target_include_directories(lang_music PUBLIC include)
target_link_libraries(lang_music PUBLIC m)
target_compile_options(lang_music PRIVATE -Wall -Wextra -O2)

add_executable(test_music tests/test_music.c)
target_link_libraries(test_music PRIVATE lang_music)
target_compile_options(test_music PRIVATE -Wall -Wextra)
add_test(NAME music COMMAND test_music)

add_executable(test_audio tests/test_audio.c)
target_link_libraries(test_audio PRIVATE lang_music)
target_compile_options(test_audio PRIVATE -Wall -Wextra -O2)
add_test(NAME audio COMMAND test_audio)

# A song written the way the language will look, in plain C for now.
add_executable(demo_song demo/demo_song.c)
target_link_libraries(demo_song PRIVATE lang_music)
target_compile_options(demo_song PRIVATE -Wall -Wextra -O2)
