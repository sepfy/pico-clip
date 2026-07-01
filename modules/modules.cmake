# Register local external modules for this app build.
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/../../modules/lib/libsrtp
  ${CMAKE_CURRENT_LIST_DIR}/../../modules/lib/libpeer
)
