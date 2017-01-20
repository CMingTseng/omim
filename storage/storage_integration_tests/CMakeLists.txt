project(storage_integration_tests)

include_directories(${OMIM_ROOT}/3party/glm)
add_definitions("-DOMIM_UNIT_TEST_WITH_QT_EVENT_LOOP")

set(
  SRC
  migrate_tests.cpp
  storage_3levels_tests.cpp
  storage_downloading_tests.cpp
  storage_group_download_tests.cpp
  storage_http_tests.cpp
  storage_update_tests.cpp
  test_defines.cpp
  test_defines.hpp
)

omim_add_test(${PROJECT_NAME} ${SRC})

omim_link_libraries(
  ${PROJECT_NAME}
  map
  drape_frontend
  routing
  search
  storage
  tracking
  traffic
  indexer
  drape
  partners_api
  platform_tests_support
  platform
  editor
  opening_hours
  geometry
  coding
  base
  freetype
  expat
  fribidi
  jansson
  protobuf
  osrm
  stats_client
  minizip
  succinct
  pugixml
  oauthcpp
  ${Qt5Widgets_LIBRARIES}
  ${Qt5Network_LIBRARIES}
  ${LIBZ}
)

link_opengl(${PROJECT_NAME})
link_qt5_core(${PROJECT_NAME})