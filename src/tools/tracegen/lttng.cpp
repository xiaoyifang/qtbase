// Copyright (C) 2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com, author Rafael Roquetto <rafael.roquetto@kdab.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "lttng.h"
#include "provider.h"
#include "helpers.h"
#include "panic.h"
#include "qtheaders.h"

#include <qfile.h>
#include <qfileinfo.h>
#include <qtextstream.h>
#include <qdebug.h>

static void writeCtfMacro(QTextStream &stream, const Tracepoint::Field &field)
{
    const QString &paramType = field.paramType;
    const QString &name = field.name;
    const QString &seqLen = field.seqLen;
    const int arrayLen = field.arrayLen;

    switch (field.backendType) {
    case Tracepoint::Field::Array:
        stream << "ctf_array(" <<paramType << ", "
               << name << ", " << name << ", " << arrayLen << ")";
        return;
    case Tracepoint::Field::Sequence:
        stream << "ctf_sequence(" << paramType
            << ", " << name << ", " << name
            << ", unsigned int, "  << seqLen << ")";
        return;
    case Tracepoint::Field::Integer:
        stream << "ctf_integer(" << paramType << ", " << name << ", " << name << ")";
        return;
    case Tracepoint::Field::IntegerHex:
    case Tracepoint::Field::Pointer:
        stream << "ctf_integer_hex(" << paramType << ", " << name << ", " << name << ")";
        return;
    case Tracepoint::Field::Float:
        stream << "ctf_float(" << paramType << ", " << name << ", " << name << ")";
        return;
    case Tracepoint::Field::String:
        stream << "ctf_string(" << name << ", " << name << ")";
        return;
    case Tracepoint::Field::QtString:
        stream << "ctf_sequence(const ushort, " << name << ", "
               << name << ".utf16(), unsigned int, " << name << ".size())";
        return;
    case Tracepoint::Field::QtByteArray:
        stream << "ctf_sequence(const char, " << name << ", "
               << name << ".constData(), unsigned int, " << name << ".size())";
        return;
    case Tracepoint::Field::QtUrl:
        stream << "ctf_sequence(const char, " << name << ", "
               << name << ".toEncoded().constData(), unsigned int, "
               << name << ".toEncoded().size())";
        return;
    case Tracepoint::Field::QtRect:
        stream << "ctf_integer(int, x, " << name << ".x()) "
               << "ctf_integer(int, y, " << name << ".y()) "
               << "ctf_integer(int, width, " << name << ".width()) "
               << "ctf_integer(int, height, " << name << ".height()) ";
        return;
    case Tracepoint::Field::QtSize:
        stream << "ctf_integer(int, width, " << name << ".width()) "
               << "ctf_integer(int, height, " << name << ".height()) ";
        return;
    case Tracepoint::Field::Unknown:
        justified_worry("Cannot deduce CTF type for '%s %s'", qPrintable(paramType),
                        qPrintable(name));
        break;
    }
}

static void writePrologue(QTextStream &stream, const QString &fileName, const Provider &provider)
{
    const QString guard = includeGuard(fileName);

    stream << "#undef TRACEPOINT_PROVIDER\n";
    stream << "#define TRACEPOINT_PROVIDER " << provider.name << "\n";
    stream << "\n";

    // include prefix text or qt headers only once
    stream << "#if !defined(" << guard << ")\n";
    stream << qtHeaders();
    stream << "\n";
    if (!provider.prefixText.isEmpty())
        stream << provider.prefixText.join(u'\n') << "\n\n";
    stream << "#endif\n\n";

    /* the first guard is the usual one, the second is required
     * by LTTNG to force the re-evaluation of TRACEPOINT_* macros
     */
    stream << "#if !defined(" << guard << ") || defined(TRACEPOINT_HEADER_MULTI_READ)\n";

    stream << "#define " << guard << "\n\n"
           << "#undef TRACEPOINT_INCLUDE\n"
           << "#define TRACEPOINT_INCLUDE \"" << fileName << "\"\n\n";

    stream << "#include <lttng/tracepoint.h>\n\n";

    const QString namespaceGuard = guard + QStringLiteral("_USE_NAMESPACE");
    stream << "#if !defined(" << namespaceGuard << ")\n"
           << "#define " << namespaceGuard << "\n"
           << "QT_USE_NAMESPACE\n"
           << "#endif // " << namespaceGuard << "\n\n";
}

static void writeEpilogue(QTextStream &stream, const QString &fileName)
{
    stream << "\n";
    stream << "#endif // " << includeGuard(fileName) << "\n"
           << "#include <lttng/tracepoint-event.h>\n"
           << "#include <private/qtrace_p.h>\n";
}

static void writeWrapper(QTextStream &stream,
        const Tracepoint &tracepoint, const QString &providerName)
{
    const QString argList = formatFunctionSignature(tracepoint.args);
    const QString paramList = formatParameterList(tracepoint.args, LTTNG);
    const QString &name = tracepoint.name;
    const QString includeGuard = QStringLiteral("TP_%1_%2").arg(providerName).arg(name).toUpper();

    /* prevents the redefinion of the inline wrapper functions
     * once LTTNG recursively includes this header file
     */
    stream << "\n"
           << "#ifndef " << includeGuard << "\n"
           << "#define " << includeGuard << "\n"
           << "QT_BEGIN_NAMESPACE\n"
           << "namespace QtPrivate {\n";

    stream << "inline void trace_" << name << "(" << argList << ")\n"
           << "{\n"
           << "    tracepoint(" << providerName << ", " << name << paramList << ");\n"
           << "}\n";

    stream << "inline void do_trace_" << name << "(" << argList << ")\n"
           << "{\n"
           << "    do_tracepoint(" << providerName << ", " << name << paramList << ");\n"
           << "}\n";

    stream << "inline bool trace_" << name << "_enabled()\n"
           << "{\n"
           << "    return tracepoint_enabled(" << providerName << ", " << name << ");\n"
           << "}\n";

    stream << "} // namespace QtPrivate\n"
           << "QT_END_NAMESPACE\n"
           << "#endif // " << includeGuard << "\n\n";
}

static void writeTracepoint(QTextStream &stream,
        const Tracepoint &tracepoint, const QString &providerName)
{
    stream  << "TRACEPOINT_EVENT(\n"
            << "    " << providerName << ",\n"
            << "    " << tracepoint.name << ",\n"
            << "    TP_ARGS(";

    const char *comma = nullptr;

    for (const Tracepoint::Argument &arg : tracepoint.args) {
        stream << comma << arg.type << ", " << arg.name;
        comma = ", ";
    }

    stream << "),\n"
        << "    TP_FIELDS(";

    const char *newline = nullptr;

    for (const Tracepoint::Field &f : tracepoint.fields) {
        stream << newline;
        writeCtfMacro(stream, f);
        newline = "\n        ";
    }

    stream << ")\n)\n\n";
}

static void writeTracepoints(QTextStream &stream, const Provider &provider)
{
    for (const Tracepoint &t : provider.tracepoints) {
        writeTracepoint(stream, t, provider.name);
        writeWrapper(stream, t, provider.name);
    }
}

void writeLttng(QFile &file, const Provider &provider)
{
    QTextStream stream(&file);

    const QString fileName = QFileInfo(file.fileName()).fileName();

    writePrologue(stream, fileName, provider);
    writeTracepoints(stream, provider);
    writeEpilogue(stream, fileName);
}
