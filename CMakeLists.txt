cmake_minimum_required(VERSION 3.20)
project(C_omposer C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# lang/   — the language (name TBD: Csus/Cdim/...): interpreter, compiler, CLI
# editor/ — C-omposer, the GTK4 editor (uses the interpreter for live mode)
add_subdirectory(editor)
