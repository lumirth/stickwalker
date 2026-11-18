include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
idf_component_register(
  SRCS ${POKEWALKER_SOURCES}
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS ${POKEWALKER_PRIVATE_INCLUDE_DIRS})
pokewalker_configure_target(${COMPONENT_LIB})
