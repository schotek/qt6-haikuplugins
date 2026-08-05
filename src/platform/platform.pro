TARGET  = qhaiku
PLUGIN_TYPE = platforms
PLUGIN_CLASS_NAME = QHaikuExpIntegrationPlugin
TEMPLATE = lib
load(qt_plugin)

QT += widgets-private core-private gui-private

LIBS += -lbe -lroot -ltracker -lgame -lGL -lGLU

exists(/etc/vos.specs) {
	# Vitruvian: Haiku API headers live in /system/develop/headers (list in
	# /etc/vos.specs) and every unit including them needs the Linux build
	# shim. BGLView is in libopengl.so here (libGL is Mesa's).
	VOS_INCLUDES = $$system(cat /etc/vos.specs)
	# vos.specs misses a few kit subdirs the plugin needs directly
	VOS_INCLUDES += -I/system/develop/headers/os/opengl \
		-I/system/develop/headers/os/game \
		-I/system/develop/headers/os/locale
	QMAKE_CFLAGS += $$VOS_INCLUDES -include LinuxBuildCompatibility.h
	QMAKE_CXXFLAGS += $$VOS_INCLUDES -include LinuxBuildCompatibility.h
	LIBS += -lopengl
}

CONFIG += plugin

CONFIG += link_pkgconfig
PKGCONFIG += freetype2

QMAKE_USE_PRIVATE += freetype

INCLUDEPATH += ../../3rdparty/simplecrypt/

SOURCES =   main.cpp \
			qhaikuapplication.cpp \
			qhaikubackingstore.cpp \
			qhaikuclipboard.cpp \
			qhaikucursor.cpp \
			qhaikuglcontext.cpp \
			qhaikuintegration.cpp \
			qhaikunativeinterface.cpp \
			qhaikuoffscreensurface.cpp \
			qhaikuplatformdialoghelpers.cpp \
			qhaikuplatformfontdatabase.cpp \
			qhaikuscreen.cpp \
			qhaikuservices.cpp \
			qhaikusystemlocale.cpp \
			qhaikusystemtrayicon.cpp \
			qhaikutheme.cpp \
			qhaikuview.cpp \
			qhaikuwindow.cpp \
            ../../3rdparty/simplecrypt/simplecrypt.cpp

HEADERS =	qhaikuapplication.h \
			qhaikubackingstore.h \
			qhaikuclipboard.h \
			qhaikucursor.h \
			qhaikuglcontext.h \
			qhaikuintegration.h \
			qhaikunativeinterface.h \
			qhaikuoffscreensurface.h \
			qhaikuplatformdialoghelpers.h \
			qhaikuplatformfontdatabase.h \
			qhaikuscreen.h \
			qhaikuservices.h \
			qhaikusystemlocale.h \
			qhaikusystemtrayicon.h \
			qhaikutheme.h \
			qhaikuview.h \
			qhaikuwindow.h

OTHER_FILES += haiku.json
