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

# ── Translations (manual to work around Qt macro issues with spaces in paths) ──
set(TS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/translations)
set(QM_DIR ${CMAKE_CURRENT_BINARY_DIR}/translations)
file(MAKE_DIRECTORY ${QM_DIR})

find_program(QT_LRELEASE_EXECUTABLE
    NAMES lrelease lrelease-qt6
    HINTS ${Qt6_DIR}/../../../bin /usr/lib/qt6/bin)

set(TS_NAMES
    rufus-qt_es.ts
    rufus-qt_pt_BR.ts
    rufus-qt_ru.ts
    rufus-qt_fr.ts
    rufus-qt_de.ts
)

set(QM_PATHS)
set(QRC_CONTENT "<!DOCTYPE RCC><RCC version=\"1.0\">\n")
set(QRC_CONTENT "${QRC_CONTENT}  <qresource prefix=\"/i18n\">\n")

foreach(ts_name ${TS_NAMES})
    string(REGEX REPLACE "\\.ts$" ".qm" qm_name ${ts_name})
    set(qm_path ${QM_DIR}/${qm_name})
    list(APPEND QM_PATHS ${qm_path})

    if(QT_LRELEASE_EXECUTABLE)
        add_custom_command(OUTPUT ${qm_path}
            COMMAND ${QT_LRELEASE_EXECUTABLE} ${TS_DIR}/${ts_name} -qm ${qm_path}
            MAIN_DEPENDENCY ${TS_DIR}/${ts_name}
            COMMENT "lrelease ${ts_name}")
    endif()

    set(QRC_CONTENT "${QRC_CONTENT}    <file alias=\"${qm_name}\">${qm_path}</file>\n")
endforeach()

set(QRC_CONTENT "${QRC_CONTENT}  </qresource>\n")
set(QRC_CONTENT "${QRC_CONTENT}</RCC>\n")

set(QRC_PATH ${CMAKE_CURRENT_BINARY_DIR}/translations.qrc)
file(WRITE ${QRC_PATH} ${QRC_CONTENT})

target_sources(rufus-qt PRIVATE ${QRC_PATH})

if(QT_LRELEASE_EXECUTABLE)
    add_custom_target(rufus-qt_qm ALL DEPENDS ${QM_PATHS})
    add_dependencies(rufus-qt rufus-qt_qm)
endif()

install(TARGETS rufus-qt RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
