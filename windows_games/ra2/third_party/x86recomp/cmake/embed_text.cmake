# cmake -DIN=<file> -DOUT=<header> -DNAME=<identifier> -P embed_text.cmake
# Writes <header> holding the text of <file> as `inline constexpr char NAME[]`.
file(READ "${IN}" _text)
file(WRITE "${OUT}" "#pragma once\ninline constexpr char ${NAME}[] = R\"EMBED(${_text})EMBED\";\n")
