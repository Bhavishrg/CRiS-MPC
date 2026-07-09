# Tests for SS_PSI/3pc
# The PSI root is on the include path so headers can be reached via "3pc/..." includes.

# ── 3PC RSS tests ─────────────────────────────────────────────────────────────
add_executable(test_shuffle3p test_shuffle3p.cpp)
target_link_libraries(test_shuffle3p PRIVATE emp-tool::emp-tool)

add_executable(test_eqz_nph test_eqz_nph.cpp)
target_link_libraries(test_eqz_nph PRIVATE emp-tool::emp-tool)

add_executable(test_shuffle_nph_2p test_shuffle_nph_2p.cpp)
target_link_libraries(test_shuffle_nph_2p PRIVATE emp-tool::emp-tool)

add_executable(test_shuffle_nph_2p_grouped test_shuffle_nph_2p_grouped.cpp)
target_link_libraries(test_shuffle_nph_2p_grouped PRIVATE emp-tool::emp-tool)
