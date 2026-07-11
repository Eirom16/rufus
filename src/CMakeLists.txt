qt_add_executable(rufus-qt
    main.cpp
    mainwindow.cpp mainwindow.h
    workers.cpp workers.h
)

target_link_libraries(rufus-qt PRIVATE
    Qt6::Widgets
)

target_include_directories(rufus-qt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

install(TARGETS rufus-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
