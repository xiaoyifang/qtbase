// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#ifndef OPTION_H
#define OPTION_H

#include <qstring.h>
#include <qdir.h>

QT_BEGIN_NAMESPACE

struct Option
{
    unsigned int headerProtection : 1;
    unsigned int copyrightHeader : 1;
    unsigned int generateImplemetation : 1;
    unsigned int generateNamespace : 1;
    unsigned int autoConnection : 1;
    unsigned int dependencies : 1;
    unsigned int limitXPM_LineLength : 1;
    unsigned int implicitIncludes: 1;
    unsigned int idBased: 1;
    unsigned int fromImports: 1;
    unsigned int forceMemberFnPtrConnectionSyntax: 1;
    unsigned int forceStringConnectionSyntax: 1;
    unsigned int useStarImports: 1;

    QString inputFile;
    QString outputFile;
    QString qrcOutputFile;
    QString indent;
    QString prefix;
    QString postfix;
    QString translateFunction;
    QString includeFile;

    Option()
        : headerProtection(1),
          copyrightHeader(1),
          generateImplemetation(0),
          generateNamespace(1),
          autoConnection(1),
          dependencies(0),
          limitXPM_LineLength(0),
          implicitIncludes(1),
          idBased(0),
          fromImports(0),
          forceMemberFnPtrConnectionSyntax(0),
          forceStringConnectionSyntax(0),
          useStarImports(0),
          prefix(QLatin1StringView("Ui_"))
    { indent.fill(u' ', 4); }

    QString messagePrefix() const
    {
        return inputFile.isEmpty() ?
               QString(QLatin1StringView("stdin")) :
               QDir::toNativeSeparators(inputFile);
    }
};

QT_END_NAMESPACE

#endif // OPTION_H
