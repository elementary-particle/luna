vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO libsdl-org/SDL_mixer
    REF "release-${VERSION}"
    SHA512 5f53ab3011e5727df51e405a687c0699e1530d4d597ab299ce8a6008a3c8295cf9170b072bf07ec49fb0af6eee757005a10cf67aa283b23575a3f58874c9b6be
    HEAD_REF main
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        aiff    SDLMIXER_AIFF
        wave    SDLMIXER_WAVE
        voc     SDLMIXER_VOC
        au      SDLMIXER_AU
        flac    SDLMIXER_FLAC
        mod     SDLMIXER_MOD
        mp3     SDLMIXER_MP3
        midi    SDLMIXER_MIDI_TIMIDITY
        opus    SDLMIXER_OPUS
        vorbis  SDLMIXER_VORBIS_VORBISFILE
        wavpack SDLMIXER_WAVPACK
    INVERTED_FEATURES
        # Disabled capabilities: Needing dependencies.
        core    SDLMIXER_GME
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DSDLMIXER_TESTS=OFF
        -DSDLMIXER_FLAC_DRFLAC=OFF
        -DSDLMIXER_MP3_DRMP3=OFF
        -DSDLMIXER_MIDI_FLUIDSYNTH=OFF
        -DSDLMIXER_DEPS_SHARED=OFF
        -DSDLMIXER_RELOCATABLE=ON
        -DSDLMIXER_STRICT=ON
        -DSDLMIXER_VENDORED=OFF
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(EXISTS "${CURRENT_PACKAGES_DIR}/cmake")
    vcpkg_cmake_config_fixup(PACKAGE_NAME SDL3_mixer CONFIG_PATH cmake)
else()
    vcpkg_cmake_config_fixup(PACKAGE_NAME SDL3_mixer CONFIG_PATH lib/cmake/SDL3_mixer)
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/debug/include"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")