# Keep the recordings with the mixer in editor, standalone games and Ship exports.
file(READ "${INPUT}" hex HEX)
set(byte "[0-9a-f][0-9a-f]")
string(REPEAT "${byte}" 24 row)
string(REGEX REPLACE "(${row})" "\\1\n" hex "${hex}")
string(REGEX REPLACE "(${byte})" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "#include \"thunder_recordings.hpp\"\n"
    "std::span<const unsigned char> vcr::embedded_${EFFECT}_recordings() noexcept {\n"
    "    static constexpr unsigned char data[] = {\n${bytes}\n};\n    return data;\n}\n")
