/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Copyright (C) 2015-2020 Gerasim Troeglazov,
** Contact: 3dEyes@gmail.com
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtGui/private/qgenericunixeventdispatcher_p.h>
#include <QtGui/private/qgenericunixfontdatabase_p.h>
#include <QtGui/private/qfreetypefontdatabase_p.h>
#include <QtGui/private/qpixmap_raster_p.h>
#include <QtGui/private/qguiapplication_p.h>
#include <private/qsimpledrag_p.h>
#include <QDir>
#include <QFile>
#include <QDirIterator>

#include <qpa/qplatformfontdatabase.h>
#include <qpa/qplatformservices.h>
#include <qpa/qplatformopenglcontext.h>

#include "qhaikuintegration.h"

QT_BEGIN_INCLUDE_NAMESPACE
extern char **environ;
QT_END_INCLUDE_NAMESPACE

QT_BEGIN_NAMESPACE

// The BApplication runs on its own spawned thread (haikuApplicationThread).
// Its id is stored here so the destructor can Quit() the app and join the
// thread before Qt tears down — otherwise the still-running BApplication
// thread races the C++ exit finalization and corrupts the heap. The plugin
// is a singleton, so a single static suffices.
static thread_id sBAppThread = -1;

QHaikuIntegration::QHaikuIntegration(const QStringList &parameters, int &argc, char **argv)
	: QObject(), QPlatformIntegration()
{
	Q_UNUSED(parameters);
	Q_UNUSED(argc);
	Q_UNUSED(argv);
	m_screen = new QHaikuScreen();
	QWindowSystemInterface::handleScreenAdded(m_screen);
    m_fontDatabase = new QHaikuPlatformFontDatabase();
    m_nativeInterface = new QHaikuNativeInterface(this);
	m_services = new QHaikuServices();
	m_clipboard = new QHaikuClipboard();
	m_haikuSystemLocale = new QHaikuSystemLocale;
	m_drag = new QSimpleDrag();
	m_openGlEnabled = isOpenGLEnabled();
}

QHaikuIntegration::~QHaikuIntegration()
{
	// Fork mode keeps the crude fast path: kill the process outright rather
	// than unwinding (checked while be_app is still valid).
	if (be_app != NULL
			&& (static_cast<HQApplication*>(be_app)->QtFlags() & Q_KILL_ON_EXIT)) {
		kill(::getpid(), SIGKILL);
	}

	// Stop the BApplication run thread and JOIN it before Qt runs its C++ exit
	// finalization. Without this the thread keeps running BLooper::Loop while
	// the main thread frees Qt objects, and the two race on the heap ->
	// "malloc_consolidate(): unaligned fastbin chunk" / Guru Meditation after
	// a few app closes.
	//
	// LockWithTimeout, not Lock(): on the normal (Qt-initiated) shutdown the
	// looper is idle and we get the lock at once. If the run thread is already
	// tearing down on its own (a B_QUIT_REQUESTED racing in), it holds the
	// looper lock and leaves it locked as it exits (Looper.cpp task_looper
	// returns locked on fTerminating) — the timeout then lets us fall through
	// to the join instead of blocking forever. We hold the lock across Quit()
	// so BApplication::Quit() just PostMessage(_QUIT_)s without the
	// "you must Lock the application" console error; Unlock() lets the run
	// thread actually dispatch _QUIT_ and return from Run(). We do NOT delete
	// the BApplication (see haikuApplicationThread). Join the spawn id
	// (sBAppThread), not be_app->Thread(), which is -1 until Loop() starts.
	if (be_app != NULL) {
		if (be_app->LockWithTimeout(2000000) == B_OK) {
			be_app->Quit();
			be_app->Unlock();
		}
		if (sBAppThread >= 0) {
			status_t st;
			while (wait_for_thread(sBAppThread, &st) == B_INTERRUPTED)
				;
			sBAppThread = -1;
		}
	}

	delete m_nativeInterface;
	delete m_fontDatabase;
	delete m_haikuSystemLocale;
	delete m_clipboard;
	delete m_drag;
	delete m_services;

	QWindowSystemInterface::handleScreenRemoved(m_screen);
}

bool QHaikuIntegration::isOpenGLEnabled()
{
	// BGLView-backed OSMesa is not validated on Vitruvian yet: opt-in only.
	if (!qEnvironmentVariableIntValue("QT_HAIKU_ENABLE_GL"))
		return false;

	app_info appInfo;
	if (be_app->GetAppInfo(&appInfo) == B_OK) {
		QStringList disabledListApps;
		disabledListApps
			<< "application/x-vnd.telegram" 		//performance issues
			<< "application/x-vnd.kotatogram";		//performance issues
		return !disabledListApps.contains(appInfo.signature, Qt::CaseInsensitive);
	}
	return true;

}

QHaikuIntegration *QHaikuIntegration::createHaikuIntegration(const QStringList& parameters, int &argc, char **argv)
{
	SimpleCrypt crypt(Q_UINT64_C(0x3de48151623423de));
	QSettings settings(QT_SETTINGS_FILENAME, QSettings::NativeFormat);
	settings.beginGroup("QPA");

	QString appSignature;

	char signature[B_MIME_TYPE_LENGTH];
	signature[0] = '\0';

	QString appPath = QCoreApplication::applicationFilePath();
	
	BFile appFile(appPath.toUtf8().constData(), B_READ_ONLY);
	if (appFile.InitCheck() == B_OK) {
		BAppFileInfo info(&appFile);
		if (info.InitCheck() == B_OK) {
			if (info.GetSignature(signature) != B_OK)
				signature[0] = '\0';
		}
	}

	if (signature[0] != '\0')
		appSignature = QLatin1String(signature);
	else
		appSignature = QLatin1String("application/x-vnd.qt6-") +
			QCoreApplication::applicationName().replace(' ', '_').remove("_x86");

	// Inject system environment (hack for QuickLaunch)
	QProcess proc;
	QStringList envList = QProcess::systemEnvironment();

	if ( envList.filter("HOME=").size() == 0 ) {
		proc.start("/bin/sh", QStringList() << "-c" << "source /boot/system/boot/SetupEnvironment; env ");
		proc.waitForFinished();
		QString resultSystemEnv(proc.readAllStandardOutput());
		envList << resultSystemEnv.split("\n");

		proc.start("/bin/sh", QStringList() << "-c" << "source /boot/home/config/settings/boot/UserSetupEnvironment; env ");
		proc.waitForFinished();
		QString resultUserEnv(proc.readAllStandardOutput());
		envList << resultUserEnv.split("\n");

		// Set XDG variables
		envList << "XDG_CONFIG_HOME=/boot/home/config/settings";
		envList << "XDG_CONFIG_DIRS=/boot/system/settings";
		envList << "XDG_CACHE_HOME=/boot/home/config/cache";
		envList << "XDG_DATA_HOME=/boot/home/config/non-packaged/data";
		envList << "XDG_DATA_DIRS=/boot/system/non-packaged/data:/boot/system/data";
	}

	thread_id my_thread;
	HQApplication *haikuApplication = NULL;

	if (be_app == NULL) {
		haikuApplication = new HQApplication(appSignature.toUtf8().constData());
		// remember below for the shutdown join (see the destructor)
		
		uint32 qtFlags = 0;
		BResources *appResource = BApplication::AppResources();
		if (appResource != NULL) {
			size_t qtFlagsTextSize = 0;
			const char *qtFlagsText = (const char*)appResource->LoadResource(B_STRING_TYPE, "QT:QPA_FLAGS", &qtFlagsTextSize);			
			if (qtFlagsText != NULL && qtFlagsTextSize > 0) {
				BString qtFlagsString(qtFlagsText);
				if (qtFlagsString.FindFirst("Q_REF_TO_ARGV") != B_ERROR)
					qtFlags |= Q_REF_TO_ARGV;
				if (qtFlagsString.FindFirst("Q_REF_TO_FORK") != B_ERROR)
					qtFlags |= Q_REF_TO_FORK;
				if (qtFlagsString.FindFirst("Q_KILL_ON_EXIT") != B_ERROR)
					qtFlags |= Q_KILL_ON_EXIT;
			}
		}
		haikuApplication->SetQtFlags(qtFlags);		

		my_thread = spawn_thread(haikuApplicationThread, "BApplication_thread", B_NORMAL_PRIORITY, (void*)haikuApplication);
		sBAppThread = my_thread;
		resume_thread(my_thread);

		if (settings.value("hide_from_deskbar", true).toBool()) {
			BMessage message;
			message.what = 'BRAQ';
			message.AddInt32("be:team", ::getpid());
			BMessenger("application/x-vnd.Be-TSKB").SendMessage(&message);
		}

		haikuApplication->UnlockLooper();
		haikuApplication->waitForRun();

		if (haikuApplication->openFiles().size() > 0) {
			// Replace argv data
			argc = 1;
			for ( const auto& fileName : haikuApplication->openFiles() ) {
				if (haikuApplication->QtFlags() & Q_REF_TO_ARGV)
					argv[argc++] = strdup(fileName.toUtf8().data());
				else
					QCoreApplication::postEvent(QCoreApplication::instance(), new QFileOpenEvent(fileName));
			}
		}

		// Rebuild environment arrays. setenv() copies name and value;
		// putenv() would store a pointer into the QByteArray temporary and
		// leave environ full of dangling entries, so HOME reads back empty
		// and QStandardPaths resolves everything under "/".
		clearenv();
		for ( const auto& envValue : envList ) {
			const QByteArray entry = envValue.toUtf8();
			const int eq = entry.indexOf('=');
			if (eq <= 0)
				continue;
			setenv(entry.left(eq).constData(),
				entry.mid(eq + 1).constData(), 1);
		}
	}

	// Enable software rendering for QML
	if (settings.value("qml_software_render", false).toBool())
		setenv("QMLSCENE_DEVICE", "softwarecontext", 0);
	settings.endGroup();

	// Proxy settings
	settings.beginGroup("Network");
	if (settings.value("use_proxy", false).toBool()) {
		QString emptyString = crypt.encryptToString(QString(""));
		if (settings.value("http_proxy_enable", false).toBool()) {
			QString username = settings.value("http_proxy_username", QString("")).toString();
			QString password = crypt.decryptToString(settings.value("http_proxy_password", emptyString).toString());
			QString scheme = settings.value("http_proxy_scheme", QString("http://")).toString();
			QString http_proxy("http_proxy=");
			http_proxy += scheme;
			if (!username.isEmpty() && !password.isEmpty())
				http_proxy += username + ":" + password + "@";
			http_proxy += settings.value("http_proxy_address", QString("")).toString() + ":";
			http_proxy += QString::number(settings.value("http_proxy_port", 8080).toInt()) + "/";
			putenv(http_proxy.toUtf8().data());
		}
		if (settings.value("https_proxy_enable", false).toBool()) {
			QString username = settings.value("https_proxy_username", QString("")).toString();
			QString password = crypt.decryptToString(settings.value("https_proxy_password", emptyString).toString());
			QString scheme = settings.value("https_proxy_scheme", QString("http://")).toString();
			QString https_proxy("https_proxy=");
			https_proxy += scheme;
			if (!username.isEmpty() && !password.isEmpty())
				https_proxy += username + ":" + password + "@";
			https_proxy += settings.value("https_proxy_address", QString("")).toString() + ":";
			https_proxy += QString::number(settings.value("https_proxy_port", 8080).toInt()) + "/";
			putenv(https_proxy.toUtf8().data());
		}
		if (settings.value("ftp_proxy_enable", false).toBool()) {
			QString username = settings.value("ftp_proxy_username", QString("")).toString();
			QString password = crypt.decryptToString(settings.value("ftp_proxy_password", emptyString).toString());
			QString scheme = settings.value("ftp_proxy_scheme", QString("http://")).toString();
			QString ftp_proxy("ftp_proxy=");
			ftp_proxy += scheme;
			if (!username.isEmpty() && !password.isEmpty())
				ftp_proxy += username + ":" + password + "@";
			ftp_proxy += settings.value("ftp_proxy_address", QString("")).toString() + ":";
			ftp_proxy += QString::number(settings.value("ftp_proxy_port", 8080).toInt()) + "/";
			putenv(ftp_proxy.toUtf8().data());
		}
		QString no_proxy = settings.value("no_proxy_list", QString("")).toString();
		if (!no_proxy.isEmpty()) {
			no_proxy = "no_proxy=\"" + no_proxy + "\"";
			putenv(no_proxy.toUtf8().data());
		}
	}
	settings.endGroup();

	// Override OpenGL/GLSL versions
	setenv("MESA_GL_VERSION_OVERRIDE","4.6", 0);
	setenv("MESA_GLSL_VERSION_OVERRIDE","460", 0);

    QHaikuIntegration *newHaikuIntegration = new QHaikuIntegration(parameters, argc, argv);
    connect(haikuApplication, SIGNAL(applicationQuit()), newHaikuIntegration, SLOT(platformAppQuit()), Qt::BlockingQueuedConnection);

    return newHaikuIntegration;
}

int32 QHaikuIntegration::haikuApplicationThread(void *data)
{
	HQApplication *app = static_cast<HQApplication*>(data);
	app->LockLooper();
	app->Run();
	// The BApplication object is deliberately NOT deleted here. ~BApplication
	// -> ~BLooper asserts the looper is locked while it tears down its child
	// handlers (Looper.cpp SetNextHandler), and on the shutdown race the
	// looper is not reliably locked at this point -> Guru Meditation
	// ("handler's looper must be locked before setting NextHandler").
	// The controlling thread joins us via wait_for_thread() (see the
	// destructor); this per-process singleton is reclaimed at process exit.
	Q_UNUSED(app);
	return B_OK;
}

bool QHaikuIntegration::platformAppQuit()
{
	if (QGuiApplicationPrivate::instance()->threadData.loadRelaxed()->eventLoops.isEmpty())
		return true;
	return QWindowSystemInterface::handleApplicationTermination<QWindowSystemInterface::SynchronousDelivery>();
}

bool QHaikuIntegration::hasCapability(QPlatformIntegration::Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps: return true;
    case MultipleWindows: return true;

    case OpenGL: return m_openGlEnabled;
    case ThreadedOpenGL: return m_openGlEnabled;
    case RasterGLSurface: return m_openGlEnabled;
    case OpenGLOnRasterSurface: return m_openGlEnabled;
    case AllGLFunctionsQueryable: return m_openGlEnabled;

    default: return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow *QHaikuIntegration::createPlatformWindow(QWindow *window) const
{
    QPlatformWindow *w = new QHaikuWindow(window);
    w->requestActivateWindow();
    return w;
}

QStringList QHaikuIntegration::themeNames() const
{
    return QStringList(QHaikuTheme::name());
}

QPlatformTheme *QHaikuIntegration::createPlatformTheme(const QString &name) const
{
    if (name == QHaikuTheme::name())
        return new QHaikuTheme(this);
    return NULL;
}

QPlatformBackingStore *QHaikuIntegration::createPlatformBackingStore(QWindow *window) const
{
    return new QHaikuBackingStore(window);
}

QPlatformOpenGLContext *QHaikuIntegration::createPlatformOpenGLContext(QOpenGLContext *context) const
{
	if (m_openGlEnabled)
		return new QHaikuGLContext(context);
	return nullptr;
}

QAbstractEventDispatcher *QHaikuIntegration::createEventDispatcher() const
{
    return createUnixEventDispatcher();
}

QPlatformFontDatabase *QHaikuIntegration::fontDatabase() const
{
    return m_fontDatabase;
}

QPlatformDrag *QHaikuIntegration::drag() const
{
    return m_drag;
}

QPlatformClipboard *QHaikuIntegration::clipboard() const
{
    return m_clipboard;
}

QPlatformServices *QHaikuIntegration::services() const
{
    return m_services;
}

QT_END_NAMESPACE
