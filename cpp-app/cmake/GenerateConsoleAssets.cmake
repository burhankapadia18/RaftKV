# Turns a Vite `dist/` tree into a constexpr StaticAsset table in a header.
#
# ALWAYS generates the header, even when the console is disabled -- an empty
# table then. That is what keeps the preprocessor out of the HTTP handler: the
# lookup simply finds nothing and answers "not built into this binary".
#
# Emits, for each asset, the raw bytes plus the pre-gzipped bytes produced by
# console/scripts/gzip-dist.mjs (`<file>.gz`, optional). The ETag is a hash of
# the raw content, so it is stable across rebuilds of identical input.

function(_kvdb_emit_byte_array OUT_VAR NAME FILE_PATH)
    file(READ "${FILE_PATH}" HEX_CONTENT HEX)
    string(LENGTH "${HEX_CONTENT}" HEX_LENGTH)
    math(EXPR BYTE_COUNT "${HEX_LENGTH} / 2")

    # Two hex chars -> "0xAB,". Wrapped every 16 bytes so the generated header
    # is diffable and does not trip compiler line-length limits.
    string(REGEX REPLACE "(..)" "0x\\1," BYTES "${HEX_CONTENT}")
    string(REGEX REPLACE "((0x..,){16})" "\\1\n    " BYTES "${BYTES}")

    set(${OUT_VAR}
        "constexpr unsigned char ${NAME}[${BYTE_COUNT}] = {\n    ${BYTES}\n};\n"
        PARENT_SCOPE)
endfunction()

function(_kvdb_content_type OUT_VAR FILE_NAME)
    # Only the types Vite actually emits. An unknown extension is
    # application/octet-stream rather than a guess: a wrong Content-Type on a
    # script is a page that silently does not run.
    if(FILE_NAME MATCHES "\\.html$")
        set(${OUT_VAR} "text/html; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.js$")
        set(${OUT_VAR} "text/javascript; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.css$")
        set(${OUT_VAR} "text/css; charset=utf-8" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.svg$")
        set(${OUT_VAR} "image/svg+xml" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.woff2$")
        set(${OUT_VAR} "font/woff2" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.json$")
        set(${OUT_VAR} "application/json" PARENT_SCOPE)
    elseif(FILE_NAME MATCHES "\\.ico$")
        set(${OUT_VAR} "image/x-icon" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "application/octet-stream" PARENT_SCOPE)
    endif()
endfunction()

# kvdb_generate_console_assets(<output-header> <dist-dir-or-empty>)
function(kvdb_generate_console_assets OUTPUT_HEADER DIST_DIR)
    set(ARRAYS "")
    set(ENTRIES "")
    set(INDEX 0)

    if(DIST_DIR)
        if(NOT EXISTS "${DIST_DIR}/index.html")
            # FATAL, never a silent skip. A node that quietly ships without a
            # console is the same class of failure as a TLS surface that
            # quietly falls back to plaintext: the operator asked for a thing
            # and got something else.
            message(FATAL_ERROR
                "KVDB_CONSOLE=ON but no built console at ${DIST_DIR} "
                "(index.html missing). Build it with `npm ci && npm run build` "
                "in console/, point -DKVDB_CONSOLE_DIST at the output, or "
                "configure with -DKVDB_CONSOLE=OFF.")
        endif()

        file(GLOB_RECURSE ASSET_FILES RELATIVE "${DIST_DIR}" "${DIST_DIR}/*")
        list(SORT ASSET_FILES)

        foreach(RELATIVE_PATH ${ASSET_FILES})
            # The .gz siblings are attached to their source, not listed.
            if(RELATIVE_PATH MATCHES "\\.gz$")
                continue()
            endif()

            set(FULL_PATH "${DIST_DIR}/${RELATIVE_PATH}")
            _kvdb_emit_byte_array(RAW_ARRAY "kConsoleRaw${INDEX}" "${FULL_PATH}")
            string(APPEND ARRAYS "${RAW_ARRAY}")

            set(GZIP_POINTER "nullptr")
            set(GZIP_SIZE "0")
            if(EXISTS "${FULL_PATH}.gz")
                _kvdb_emit_byte_array(GZIP_ARRAY "kConsoleGzip${INDEX}"
                                      "${FULL_PATH}.gz")
                string(APPEND ARRAYS "${GZIP_ARRAY}")
                set(GZIP_POINTER "kConsoleGzip${INDEX}")
                set(GZIP_SIZE "sizeof(kConsoleGzip${INDEX})")
            endif()

            file(SHA256 "${FULL_PATH}" CONTENT_HASH)
            string(SUBSTRING "${CONTENT_HASH}" 0 16 SHORT_HASH)
            _kvdb_content_type(CONTENT_TYPE "${RELATIVE_PATH}")

            # index.html is reachable at "/console/" (its URL never changes, so
            # it revalidates); everything else keeps its hashed name and may be
            # cached forever.
            if(RELATIVE_PATH STREQUAL "index.html")
                set(REQUEST_PATH "/console/")
                set(IMMUTABLE "false")
            else()
                set(REQUEST_PATH "/console/${RELATIVE_PATH}")
                set(IMMUTABLE "true")
            endif()

            string(APPEND ENTRIES
                "    {\"${REQUEST_PATH}\", \"${CONTENT_TYPE}\", "
                "kConsoleRaw${INDEX}, sizeof(kConsoleRaw${INDEX}), "
                "${GZIP_POINTER}, ${GZIP_SIZE}, "
                "\"\\\"${SHORT_HASH}\\\"\", ${IMMUTABLE}},\n")

            math(EXPR INDEX "${INDEX} + 1")
        endforeach()
    endif()

    if(INDEX EQUAL 0)
        # An empty table still has to be a valid array, and a zero-length array
        # is ill-formed in C++ -- so use a null pointer and a zero count.
        set(TABLE_BODY
            "inline constexpr const StaticAsset *kConsoleAssets = nullptr;\ninline constexpr std::size_t kConsoleAssetCount = 0;\n")
    else()
        set(TABLE_BODY
            "inline constexpr StaticAsset kConsoleAssetTable[] = {\n${ENTRIES}};\ninline constexpr const StaticAsset *kConsoleAssets = kConsoleAssetTable;\ninline constexpr std::size_t kConsoleAssetCount =\n    sizeof(kConsoleAssetTable) / sizeof(kConsoleAssetTable[0]);\n")
    endif()

    file(WRITE "${OUTPUT_HEADER}"
"// GENERATED by cmake/GenerateConsoleAssets.cmake. Do not edit.
//
// ${INDEX} console asset(s) embedded. An empty table is a valid configuration
// (KVDB_CONSOLE=OFF): the handler then finds nothing and answers \"console not
// built into this binary\", which is why nothing here needs a #ifdef.
#pragma once

#include <cstddef>

#include \"network/static_assets.hpp\"

namespace kvdb {

${ARRAYS}
${TABLE_BODY}
} // namespace kvdb
")

    message(STATUS "Console assets: ${INDEX} file(s) embedded")
endfunction()
