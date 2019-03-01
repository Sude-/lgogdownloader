/* This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details. */

#ifndef GUI_LOGIN_H
#define GUI_LOGIN_H

#include "config.h"
#include "util.h"
#include "globals.h"

#include <QObject>
#include <QWebEngineCookieStore>
#include <iostream>
#include <vector>

class GuiLogin : public QObject
{
    Q_OBJECT

    public:
        GuiLogin();
        virtual ~GuiLogin();

        void Login();
        std::string getCode();
        std::vector<std::string> getCookies();

    private:
        QWebEngineCookieStore *cookiestore;
        std::vector<std::string> cookies;
        std::string auth_code;

    public slots:
        void loadFinished(bool success);
        void cookieAdded(const QNetworkCookie &cookie);
};

#endif // GUI_LOGIN_H
