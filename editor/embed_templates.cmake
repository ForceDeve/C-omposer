# Turns editor/templates/*.cmp into C strings (run at build time).
file(WRITE ${OUT} "/* generated from editor/templates: edit those files, not this one */\n")
foreach(name ${NAMES})
    file(READ ${DIR}/${name}.cmp hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    file(APPEND ${OUT} "const char TEMPLATE_${name}[] = {${hex}0x00};\n")
endforeach()
