/*
 * SPDX-FileCopyrightText: 2014-2024 Megan Conkle <megan.conkle@kdemail.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <QWindow>

#include <KAboutData>
#include <KToolTipHelper>

#include "logging.h"
#include "mainwindow.h"
#include "settings/appsettings.h"

int main(int argc, char *argv[])
{
    // Set up customized logging.
    qSetMessagePattern(
        "[%{time process} %{pid} %{appname} %{if-category} %{category}%{endif}] "
        "%{if-debug}DEBUG   %{endif}"
        "%{if-info}INFO    %{endif}"
        "%{if-warning}WARNING %{endif}"
        "%{if-critical}CRITICAL%{endif}"
        "%{if-fatal}FATAL   %{endif}"
        "%{if-debug}  %{function}():%{endif}"
        "  %{message}"
        "%{if-debug} (%{file}:%{line})%{endif}");
    qInstallMessageHandler(ghostwriter::logMessage);

    bool disableGPU = false;

    // Unfortunately, we must preparse the arguments for the --disable-gpu
    // option rather than using QCommandLineParser since we must set the
    // software rendering attribute before creating the QApplication.
    //
    for (int i = 0; i < argc; i++) {
        if (0 == strcmp(argv[i], "--disable-gpu")) {
            disableGPU = true;
            break;
        }
    }

    if (disableGPU) {
        QCoreApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

#if defined(Q_OS_WIN)
    // For Qt 5, use ANGLE instead of OpenGL to bypass bug where full screen
    // windows under Windows 10 and OpenGL will not show menus from the menu
    // bar (or any other popup menus).  For Qt 6, this is option is no longer
    // available, so allow the user to pass in the software OpenGL option if
    // desired. (Note: Software rendering can be buggy, so leave it optional).
    // Sadly, the full screen OpenGL workaround in Qt's documentation does not
    // actually work.
    //
    // Thank you, Microsoft (and now Qt for removing ANGLE), you made my day.
    //
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    if (!disableGPU) {
        QCoreApplication::setAttribute(Qt::AA_UseOpenGLES, true);
    }
#endif
#endif

    // Disable icons in menus for now, since matching their colors to the
    // current theme is not supported yet.
    // QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);

    QApplication app(argc, argv);

    qApp->installEventFilter(KToolTipHelper::instance());

#if defined(Q_OS_LINUX)
    QGuiApplication::setDesktopFileName("moogle");
#endif

    KAboutData aboutData("moogle", QCoreApplication::translate("main", "moogle"), APPVERSION);

    // ".local" is reserved and never resolves publicly, so it cannot collide
    // with a real organization's settings or bundle identifiers.
    aboutData.setOrganizationDomain("moogle.local");
    aboutData.setShortDescription(QCoreApplication::translate("main", "moogle markdown: a Markdown editor, kupo!"));

    aboutData.setOtherText("<img src=\":/resources/banner.png\">");
    aboutData.addAuthor("Megan Conkle", QCoreApplication::translate("main", "Original ghostwriter developer"), "megan.conkle@kdemail.net");
    aboutData.addCredit("Zerox",
                        QCoreApplication::translate("main", "Ripped the XIII Moogle model (Kingdom Hearts 358/2 Days) used for the dark mode mascot"),
                        QString(),
                        "https://www.spriters-resource.com");
    aboutData.addCredit(QCoreApplication::translate("main", "The Models Resource"),
                        QCoreApplication::translate("main", "Source of the Kingdom Hearts (PS2) Moogle model used for the light mode mascot"),
                        QString(),
                        "https://models.spriters-resource.com");
    aboutData.addCredit("Graeme Gott",
        QCoreApplication::translate("main",
            "FocusWriter developer, whose Qt code mentored me"),
        "graeme@gottcode.org",
        "gottcode.org");
    aboutData.addCredit("Dmitry Shachnev",
        QCoreApplication::translate("main",
            "ReText developer, whose algorithms helped immensely"),
        QString(),
        "https://github.com/retext-project/retext");
    aboutData.addCredit("Gabriel M. Beddingfield",
        QCoreApplication::translate("main",
            "StretchPlayer developer, whose application showed me how to make frameless windows in Qt"),
        QString(),
        "https://www.teuton.org/~gabriel/stretchplayer/");
    aboutData.addCredit("Wolf Vollprecht",
        QCoreApplication::translate("main",
            "UberWriter (now Apostrophe) developer, for providing inspiration"),
        QString(),
        "https://www.wolfvollprecht.de");
    aboutData.addCredit(QCoreApplication::translate("main", "Other Contributors"),
        QCoreApplication::translate("main",
            "Everyone who provided translations, documentation, bug fixes, or new features over the years"),
        QString(),
        QString());
    aboutData.addComponent("cmark-gfm", 
        QCoreApplication::translate("main",
            "An extended version of the C reference implementation of CommonMark"),
        QString(),
        "https://github.com/github/cmark-gfm");
    aboutData.addComponent("React", QCoreApplication::translate("main", "A JavaScript library for building user interfaces"), QString(), "https://reactjs.org");
    aboutData.addComponent("MathJax", 
        QCoreApplication::translate("main",
            "A JavaScript display engine for mathematics"),
        QString(),
        "https://www.mathjax.org/");
    aboutData.setLicense(KAboutLicense::GPL_V3);
    aboutData.setCopyrightStatement(QCoreApplication::translate("main",
                                                                "Copyright 2014-%1 The ghostwriter team and moogle contributors.\n"
                                                                "Moogle is a character owned by Square Enix; this project is not affiliated with Square Enix.")
                                        .arg(QDateTime::currentDateTime().date().year()));
    aboutData.setHomepage("https://github.com/KDE/ghostwriter");
    aboutData.setDesktopFileName("moogle");

    // Set the application metadata.
    KAboutData::setApplicationData(aboutData);

    // Call this to force settings initialization before the application
    // fully launches.
    //
    ghostwriter::AppSettings::instance();

    QString filePath = QString();

    QCommandLineParser clParser;
    aboutData.setupCommandLine(&clParser);
    clParser.setApplicationDescription(QCoreApplication::translate("main", "Welcome to moogle, kupo!"));
    clParser.addPositionalArgument("file",
        QCoreApplication::translate("main", "(Optional) File to open."));

    QCommandLineOption renderingOption("disable-gpu",
        QCoreApplication::translate("main", "Disables GPU acceleration."));

    clParser.addOption(renderingOption);
    clParser.process(app);
    aboutData.processCommandLine(&clParser);

    QStringList posArgs = clParser.positionalArguments();

    // Prefer the bundled mascot icon so the window icon is correct even when
    // the application has not been installed into the system icon theme.
    QIcon appIcon(QStringLiteral(":/resources/icons/sc-apps-moogle.svg"));

    if (appIcon.isNull()) {
        qWarning().noquote() << R"({"context":"appmain","message":"bundled app icon missing, falling back to icon theme"})";
        appIcon = QIcon::fromTheme(QStringLiteral("moogle"));
    }

    app.setWindowIcon(appIcon);

    if (posArgs.size() > 0) {
        filePath = posArgs.first();
    }

    // Note: --disable-gpu option was already processed. We added it here
    //       only so it is displayed in the help output.
    ghostwriter::MainWindow window(filePath);

    window.show();
    return app.exec();
}
