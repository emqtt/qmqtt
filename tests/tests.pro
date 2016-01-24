QT += widgets network testlib
QT -= gui

TARGET = qmqtt_tests

DEFINES += QMQTT_LIBRARY_TESTS
INCLUDEPATH += ../src
LIBS += -L../src -lqmqtt

SOURCES += \
    clienttest.cpp \
    tcpserver.cpp \
    main.cpp \
    customprinter.cpp \
    networktest.cpp \
    messagetest.cpp \
    frametest.cpp \
    sockettest.cpp

HEADERS += \
    tcpserver.h \
    customprinter.h \
    networkmock.h \
    socketmock.h \
    timermock.h

INCLUDEPATH += \
    ../gtest/googletest/googletest/include \
    ../gtest/googletest/googlemock/include
LIBS += -L../gtest -lgtest

unix:!NO_UNIT_TESTS:!NO_RUN_UNIT_TESTS: {
    unit_tests.target = all
    unit_tests.commands = \
        LD_LIBRARY_PATH=$${OUT_PWD}/../gtest:$${OUT_PWD}/../src \
        $${OUT_PWD}/qmqtt_tests
    QMAKE_EXTRA_TARGETS += unit_tests
}
