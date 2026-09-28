# Embed a text file in C as a NUL-terminated byte array.
#
#   cmake -DIN=<file> -DOUT=<header> -DNAME=<identifier> -P embed_text.cmake
#
# Run at build time (add_custom_command), so the header follows the file.
file(READ "${IN}" content HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${content}")
get_filename_component(in_name "${IN}" NAME)
# Always written: the output must be newer than the file it came from, or the
# rule never settles.
file(WRITE "${OUT}"
     "/* Generated at build time from ${in_name} (cmake/embed_text.cmake): do not edit. */\n"
     "static const char ${NAME}[] = {${bytes}0x00};\n")
