/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#include "gui_login.h"

#include <QApplication>
#include <QtWidgets/QWidget>
#include <QtWebEngineWidgets/QWebEngineView>
#include <QStyle>
#include <QLayout>
#include <QDesktopWidget>
#include <QWebEngineProfile>

GuiLogin::GuiLogin()
{
    // constructor
}

GuiLogin::~GuiLogin()
{
    // destructor
}

void GuiLogin::loadFinished(bool success)
{
    QWebEngineView *view = qobject_cast<QWebEngineView*>(sender());
    std::string url = view->page()->url().toString().toUtf8().constData();
    if (success && url.find("https://embed.gog.com/on_login_success") != std::string::npos)
    {
        std::string find_str = "code=";
        auto pos = url.find(find_str);
        if (pos != std::string::npos)
        {
            pos += find_str.length();
            std::string code;
            code.assign(url.begin()+pos, url.end());
            if (!code.empty())
            {
                this->auth_code = code;
                QCoreApplication::exit();
            }
        }
    }
}

void GuiLogin::cookieAdded(const QNetworkCookie& cookie)
{
    std::string raw_cookie = cookie.toRawForm().toStdString();
    if (!raw_cookie.empty())
    {
        std::string set_cookie = "Set-Cookie: " + raw_cookie;
        bool duplicate = false;
        for (auto cookie : this->cookies)
        {
            if (set_cookie == cookie)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            this->cookies.push_back(set_cookie);
    }
}

void GuiLogin::Login()
{
    QByteArray redirect_uri = QUrl::toPercentEncoding(QString::fromStdString(Globals::galaxyConf.getRedirectUri()));
    std::string auth_url = "https://auth.gog.com/auth?client_id=" + Globals::galaxyConf.getClientId() + "&redirect_uri=" + redirect_uri.toStdString() + "&response_type=code";
    QUrl url = QString::fromStdString(auth_url);

    std::vector<char> version_string(
        Globals::globalConfig.sVersionString.c_str(),
        Globals::globalConfig.sVersionString.c_str() + Globals::globalConfig.sVersionString.size() + 1
    );

    int argc = 1;
    char *argv[] = {&version_string[0]};
    QApplication app(argc, argv);

    QWidget window;
    QVBoxLayout *layout = new QVBoxLayout;
    QSize window_size(440, 540);

    window.setGeometry(
        QStyle::alignedRect(
            Qt::LeftToRight,
            Qt::AlignCenter,
            window_size,
            qApp->desktop()->availableGeometry()
        )
    );

    QWebEngineView *webengine = new QWebEngineView(&window);
    layout->addWidget(webengine);
    QWebEngineProfile profile;
    profile.setHttpUserAgent(QString::fromStdString(Globals::globalConfig.curlConf.sUserAgent));
    QWebEnginePage page(&profile);
    cookiestore = profile.cookieStore();

    QObject::connect(
        webengine, SIGNAL(loadFinished(bool)),
        this, SLOT(loadFinished(bool))
    );

    QObject::connect(
        this->cookiestore, SIGNAL(cookieAdded(const QNetworkCookie&)),
        this, SLOT(cookieAdded(const QNetworkCookie&))
    );

    webengine->resize(window.frameSize());
    webengine->setPage(&page);
    webengine->setUrl(url);

    window.setLayout(layout);
    window.show();

    app.exec();
}

std::string GuiLogin::getCode()
{
    return this->auth_code;
}

std::vector<std::string> GuiLogin::getCookies()
{
    return this->cookies;
}

#include "moc_gui_login.cpp"
