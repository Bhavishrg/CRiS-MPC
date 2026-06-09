# Tests for SS_PSI/3pc
# The PSI root is on the include path so headers can be reached via "3pc/..." includes.

# ── 3PC RSS tests ─────────────────────────────────────────────────────────────
add_executable(test_shuffle3p test_shuffle3p.cpp)
target_link_libraries(test_shuffle3p PRIVATE emp-tool::emp-tool)

