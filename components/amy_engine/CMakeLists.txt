set(AMY_SRC_DIR "${CMAKE_SOURCE_DIR}/third_party/amy/src")

set(AMY_CORE_SRCS
    "${AMY_SRC_DIR}/algorithms.c"
    "${AMY_SRC_DIR}/amy.c"
    "${AMY_SRC_DIR}/envelope.c"
    "${AMY_SRC_DIR}/examples.c"
    "${AMY_SRC_DIR}/parse.c"
    "${AMY_SRC_DIR}/filters.c"
    "${AMY_SRC_DIR}/oscillators.c"
    "${AMY_SRC_DIR}/pcm.c"
    "${AMY_SRC_DIR}/interp_partials.c"
    "${AMY_SRC_DIR}/custom.c"
    "${AMY_SRC_DIR}/delay.c"
    "${AMY_SRC_DIR}/log2_exp2.c"
    "${AMY_SRC_DIR}/patches.c"
    "${AMY_SRC_DIR}/transfer.c"
    "${AMY_SRC_DIR}/sequencer.c"
    "${AMY_SRC_DIR}/instrument.c"
    "${AMY_SRC_DIR}/amy_midi.c"
    "${AMY_SRC_DIR}/api.c"
    "${AMY_SRC_DIR}/midi_mappings.c"
    "${AMY_SRC_DIR}/i2s.c"
)

idf_component_register(
    SRCS
        "amy_engine.c"
        ${AMY_CORE_SRCS}
    EMBED_FILES
        "../../sounds/sleepwalk.wav"
    INCLUDE_DIRS
        "include"
        "${AMY_SRC_DIR}"
    REQUIRES
        freertos
        esp_timer
        driver
        nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE AMY_WAVETABLE)
