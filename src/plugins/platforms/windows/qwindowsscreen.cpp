// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qwindowsscreen.h"
#include "qwindowscontext.h"
#include "qwindowswindow.h"
#include "qwindowsintegration.h"
#include "qwindowscursor.h"
#include "qwindowstheme.h"

#include <QtCore/qt_windows.h>

#include <QtCore/qsettings.h>
#include <QtGui/qpixmap.h>
#include <QtGui/qguiapplication.h>
#include <qpa/qwindowsysteminterface.h>
#include <private/qhighdpiscaling_p.h>
#include <private/qwindowsfontdatabasebase_p.h>
#include <QtGui/qscreen.h>

#include <QtCore/qdebug.h>

#include <shellscalingapi.h>

QT_BEGIN_NAMESPACE

using namespace Qt::StringLiterals;

static inline QDpi deviceDPI(HDC hdc)
{
    return QDpi(GetDeviceCaps(hdc, LOGPIXELSX), GetDeviceCaps(hdc, LOGPIXELSY));
}

static inline QDpi monitorDPI(HMONITOR hMonitor)
{
    UINT dpiX;
    UINT dpiY;
    if (SUCCEEDED(GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        return QDpi(dpiX, dpiY);
    return {0, 0};
}

static bool getPathInfo(const MONITORINFOEX &viewInfo, DISPLAYCONFIG_PATH_INFO *pathInfo)
{
    // We might want to consider storing adapterId/id from DISPLAYCONFIG_PATH_TARGET_INFO.
    std::vector<DISPLAYCONFIG_PATH_INFO> pathInfos;
    std::vector<DISPLAYCONFIG_MODE_INFO> modeInfos;

    // Fetch paths
    LONG result;
    UINT32 numPathArrayElements;
    UINT32 numModeInfoArrayElements;
    do {
        // QueryDisplayConfig documentation doesn't say the number of needed elements is updated
        // when the call fails with ERROR_INSUFFICIENT_BUFFER, so we need a separate call to
        // look up the needed buffer sizes.
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPathArrayElements,
                                        &numModeInfoArrayElements) != ERROR_SUCCESS) {
            return false;
        }
        pathInfos.resize(numPathArrayElements);
        modeInfos.resize(numModeInfoArrayElements);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPathArrayElements, pathInfos.data(),
                                    &numModeInfoArrayElements, modeInfos.data(), nullptr);
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS)
        return false;

    // Find path matching monitor name
    for (uint32_t p = 0; p < numPathArrayElements; p++) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME deviceName;
        deviceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        deviceName.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
        deviceName.header.adapterId = pathInfos[p].sourceInfo.adapterId;
        deviceName.header.id = pathInfos[p].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&deviceName.header) == ERROR_SUCCESS) {
            if (wcscmp(viewInfo.szDevice, deviceName.viewGdiDeviceName) == 0) {
                *pathInfo = pathInfos[p];
                return true;
            }
        }
    }

    return false;
}

#if 0
// Needed later for HDR support
static float getMonitorSDRWhiteLevel(DISPLAYCONFIG_PATH_TARGET_INFO *targetInfo)
{
    const float defaultSdrWhiteLevel = 200.0;
    if (!targetInfo)
        return defaultSdrWhiteLevel;

    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(DISPLAYCONFIG_SDR_WHITE_LEVEL);
    whiteLevel.header.adapterId = targetInfo->adapterId;
    whiteLevel.header.id = targetInfo->id;
    if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS)
        return defaultSdrWhiteLevel;
    return whiteLevel.SDRWhiteLevel * 80.0 / 1000.0;
}
#endif

using WindowsScreenDataList = QList<QWindowsScreenData>;

static bool monitorData(HMONITOR hMonitor, QWindowsScreenData *data)
{
    MONITORINFOEX info;
    memset(&info, 0, sizeof(MONITORINFOEX));
    info.cbSize = sizeof(MONITORINFOEX);
    if (GetMonitorInfo(hMonitor, &info) == FALSE)
        return false;

    data->hMonitor = hMonitor;
    data->geometry = QRect(QPoint(info.rcMonitor.left, info.rcMonitor.top), QPoint(info.rcMonitor.right - 1, info.rcMonitor.bottom - 1));
    data->availableGeometry = QRect(QPoint(info.rcWork.left, info.rcWork.top), QPoint(info.rcWork.right - 1, info.rcWork.bottom - 1));
    data->deviceName = QString::fromWCharArray(info.szDevice);
    DISPLAYCONFIG_PATH_INFO pathInfo = {};
    const bool hasPathInfo = getPathInfo(info, &pathInfo);
    if (hasPathInfo) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME deviceName = {};
        deviceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        deviceName.header.size = sizeof(DISPLAYCONFIG_TARGET_DEVICE_NAME);
        deviceName.header.adapterId = pathInfo.targetInfo.adapterId;
        deviceName.header.id = pathInfo.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&deviceName.header) == ERROR_SUCCESS)
            data->name = QString::fromWCharArray(deviceName.monitorFriendlyDeviceName);
    }
    if (data->name.isEmpty())
        data->name = data->deviceName;
    if (data->deviceName == u"WinDisc") {
        data->flags |= QWindowsScreenData::LockScreen;
    } else {
        if (const HDC hdc = CreateDC(info.szDevice, nullptr, nullptr, nullptr)) {
            const QDpi dpi = monitorDPI(hMonitor);
            data->dpi = dpi.first > 0 ? dpi : deviceDPI(hdc);
            data->depth = GetDeviceCaps(hdc, BITSPIXEL);
            data->format = data->depth == 16 ? QImage::Format_RGB16 : QImage::Format_RGB32;
            data->physicalSizeMM = QSizeF(GetDeviceCaps(hdc, HORZSIZE), GetDeviceCaps(hdc, VERTSIZE));
            const int refreshRate = GetDeviceCaps(hdc, VREFRESH);
            if (refreshRate > 1) // 0,1 means hardware default.
                data->refreshRateHz = refreshRate;
            DeleteDC(hdc);
        } else {
            qWarning("%s: Unable to obtain handle for monitor '%s', defaulting to %g DPI.",
                     __FUNCTION__, qPrintable(data->deviceName),
                     data->dpi.first);
        } // CreateDC() failed
    } // not lock screen

    // ### We might want to consider storing adapterId/id from DISPLAYCONFIG_PATH_TARGET_INFO,
    // if we are going to use DISPLAYCONFIG lookups more.
    if (hasPathInfo) {
        switch (pathInfo.targetInfo.rotation) {
        case DISPLAYCONFIG_ROTATION_IDENTITY:
            data->orientation = Qt::LandscapeOrientation;
            break;
        case DISPLAYCONFIG_ROTATION_ROTATE90:
            data->orientation = Qt::PortraitOrientation;
            break;
        case DISPLAYCONFIG_ROTATION_ROTATE180:
            data->orientation = Qt::InvertedLandscapeOrientation;
            break;
        case DISPLAYCONFIG_ROTATION_ROTATE270:
            data->orientation = Qt::InvertedPortraitOrientation;
            break;
        case DISPLAYCONFIG_ROTATION_FORCE_UINT32:
            Q_UNREACHABLE();
            break;
        }
        if (pathInfo.targetInfo.refreshRate.Numerator && pathInfo.targetInfo.refreshRate.Denominator)
            data->refreshRateHz = static_cast<qreal>(pathInfo.targetInfo.refreshRate.Numerator)
                                / pathInfo.targetInfo.refreshRate.Denominator;
    } else {
        data->orientation = data->geometry.height() > data->geometry.width()
                          ? Qt::PortraitOrientation
                          : Qt::LandscapeOrientation;
    }
    // EnumDisplayMonitors (as opposed to EnumDisplayDevices) enumerates only
    // virtual desktop screens.
    data->flags |= QWindowsScreenData::VirtualDesktop;
    if (info.dwFlags & MONITORINFOF_PRIMARY) {
        data->flags |= QWindowsScreenData::PrimaryScreen;
        if ((data->flags & QWindowsScreenData::LockScreen) == 0)
            QWindowsFontDatabase::setDefaultVerticalDPI(data->dpi.second);
    }
    return true;
}

// from monitorData, taking WindowsScreenDataList as LPARAM
BOOL QT_WIN_CALLBACK monitorEnumCallback(HMONITOR hMonitor, HDC, LPRECT, LPARAM p)
{
    QWindowsScreenData data;
    if (monitorData(hMonitor, &data)) {
        auto *result = reinterpret_cast<WindowsScreenDataList *>(p);
        auto it = std::find_if(result->rbegin(), result->rend(),
            [&data](QWindowsScreenData i){ return i.name == data.name; });
        if (it != result->rend()) {
            int previousIndex = 1;
            if (it->deviceIndex.has_value())
                previousIndex = it->deviceIndex.value();
            else
                (*it).deviceIndex = 1;
            data.deviceIndex = previousIndex + 1;
        }
        // QWindowSystemInterface::handleScreenAdded() documentation specifies that first
        // added screen will be the primary screen, so order accordingly.
        // Note that the side effect of this policy is that there is no way to change primary
        // screen reported by Qt, unless we want to delete all existing screens and add them
        // again whenever primary screen changes.
        if (data.flags & QWindowsScreenData::PrimaryScreen)
            result->prepend(data);
        else
            result->append(data);
    }
    return TRUE;
}

static inline WindowsScreenDataList monitorData()
{
    WindowsScreenDataList result;
    EnumDisplayMonitors(nullptr, nullptr, monitorEnumCallback, reinterpret_cast<LPARAM>(&result));
    return result;
}

#ifndef QT_NO_DEBUG_STREAM
static QDebug operator<<(QDebug dbg, const QWindowsScreenData &d)
{
    QDebugStateSaver saver(dbg);
    dbg.nospace();
    dbg.noquote();
    dbg << "Screen \"" << d.name << "\" "
        << d.geometry.width() << 'x' << d.geometry.height() << '+' << d.geometry.x() << '+' << d.geometry.y()
        << " avail: "
        << d.availableGeometry.width() << 'x' << d.availableGeometry.height() << '+' << d.availableGeometry.x() << '+' << d.availableGeometry.y()
        << " physical: " << d.physicalSizeMM.width() << 'x' << d.physicalSizeMM.height()
        << " DPI: " << d.dpi.first << 'x' << d.dpi.second << " Depth: " << d.depth
        << " Format: " << d.format
        << " hMonitor: " << d.hMonitor
        << " device name: " << d.deviceName;
    if (d.flags & QWindowsScreenData::PrimaryScreen)
        dbg << " primary";
    if (d.flags & QWindowsScreenData::VirtualDesktop)
        dbg << " virtual desktop";
    if (d.flags & QWindowsScreenData::LockScreen)
        dbg << " lock screen";
    return dbg;
}
#endif // !QT_NO_DEBUG_STREAM

/*!
    \class QWindowsScreen
    \brief Windows screen.
    \sa QWindowsScreenManager
    \internal
*/

QWindowsScreen::QWindowsScreen(const QWindowsScreenData &data) :
    m_data(data)
#ifndef QT_NO_CURSOR
    , m_cursor(new QWindowsCursor(this))
#endif
{
}

QString QWindowsScreen::name() const
{
    return m_data.deviceIndex.has_value()
               ? (u"%1 (%2)"_s).arg(m_data.name, QString::number(m_data.deviceIndex.value()))
               : m_data.name;
}

Q_GUI_EXPORT QPixmap qt_pixmapFromWinHBITMAP(HBITMAP bitmap, int hbitmapFormat = 0);

QPixmap QWindowsScreen::grabWindow(WId window, int xIn, int yIn, int width, int height) const
{
    QSize windowSize;
    int x = xIn;
    int y = yIn;
    HWND hwnd = reinterpret_cast<HWND>(window);
    if (hwnd) {
        RECT r;
        GetClientRect(hwnd, &r);
        windowSize = QSize(r.right - r.left, r.bottom - r.top);
    } else {
        // Grab current screen. The client rectangle of GetDesktopWindow() is the
        // primary screen, but it is possible to grab other screens from it.
        hwnd = GetDesktopWindow();
        const QRect screenGeometry = geometry();
        windowSize = screenGeometry.size();
        x += screenGeometry.x();
        y += screenGeometry.y();
    }

    if (width < 0)
        width = windowSize.width() - xIn;
    if (height < 0)
        height = windowSize.height() - yIn;

    // Create and setup bitmap
    HDC display_dc = GetDC(nullptr);
    HDC bitmap_dc = CreateCompatibleDC(display_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(display_dc, width, height);
    HGDIOBJ null_bitmap = SelectObject(bitmap_dc, bitmap);

    // copy data
    HDC window_dc = GetDC(hwnd);
    BitBlt(bitmap_dc, 0, 0, width, height, window_dc, x, y, SRCCOPY | CAPTUREBLT);

    // clean up all but bitmap
    ReleaseDC(hwnd, window_dc);
    SelectObject(bitmap_dc, null_bitmap);
    DeleteDC(bitmap_dc);

    const QPixmap pixmap = qt_pixmapFromWinHBITMAP(bitmap);

    DeleteObject(bitmap);
    ReleaseDC(nullptr, display_dc);

    return pixmap;
}

/*!
    \brief Find a top level window taking the flags of ChildWindowFromPointEx.
*/

QWindow *QWindowsScreen::topLevelAt(const QPoint &point) const
{
    QWindow *result = nullptr;
    if (QWindow *child = QWindowsScreen::windowAt(point, CWP_SKIPINVISIBLE))
        result = QWindowsWindow::topLevelOf(child);
    if (QWindowsContext::verbose > 1)
        qCDebug(lcQpaScreen) <<__FUNCTION__ << point << result;
    return result;
}

QWindow *QWindowsScreen::windowAt(const QPoint &screenPoint, unsigned flags)
{
    QWindow* result = nullptr;
    if (QPlatformWindow *bw = QWindowsContext::instance()->
            findPlatformWindowAt(GetDesktopWindow(), screenPoint, flags))
        result = bw->window();
    if (QWindowsContext::verbose > 1)
        qCDebug(lcQpaScreen) <<__FUNCTION__ << screenPoint << " returns " << result;
    return result;
}

/*!
    \brief Determine siblings in a virtual desktop system.

    Self is by definition a sibling, else collect all screens
    within virtual desktop.
*/

QList<QPlatformScreen *> QWindowsScreen::virtualSiblings() const
{
    QList<QPlatformScreen *> result;
    if (m_data.flags & QWindowsScreenData::VirtualDesktop) {
        const QWindowsScreenManager::WindowsScreenList screens
            = QWindowsContext::instance()->screenManager().screens();
        for (QWindowsScreen *screen : screens) {
            if (screen->data().flags & QWindowsScreenData::VirtualDesktop)
                result.push_back(screen);
        }
    } else {
        result.push_back(const_cast<QWindowsScreen *>(this));
    }
    return result;
}

/*!
    \brief Notify QWindowSystemInterface about changes of a screen and synchronize data.
*/

void QWindowsScreen::handleChanges(const QWindowsScreenData &newData)
{
    m_data.physicalSizeMM = newData.physicalSizeMM;

    if (m_data.hMonitor != newData.hMonitor) {
        qCDebug(lcQpaScreen) << "Monitor" << m_data.name
            << "has had its hMonitor handle changed from"
            << m_data.hMonitor << "to" << newData.hMonitor;
        m_data.hMonitor = newData.hMonitor;
    }

    // QGuiApplicationPrivate::processScreenGeometryChange() checks and emits
    // DPI and orientation as well, so, assign new values and emit DPI first.
    const bool geometryChanged = m_data.geometry != newData.geometry
        || m_data.availableGeometry != newData.availableGeometry;
    const bool dpiChanged = !qFuzzyCompare(m_data.dpi.first, newData.dpi.first)
        || !qFuzzyCompare(m_data.dpi.second, newData.dpi.second);
    const bool orientationChanged = m_data.orientation != newData.orientation;
    m_data.dpi = newData.dpi;
    m_data.orientation = newData.orientation;
    m_data.geometry = newData.geometry;
    m_data.availableGeometry = newData.availableGeometry;

    if (dpiChanged) {
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(screen(),
                                                                     newData.dpi.first,
                                                                     newData.dpi.second);
    }
    if (orientationChanged)
       QWindowSystemInterface::handleScreenOrientationChange(screen(), newData.orientation);
    if (geometryChanged) {
        QWindowSystemInterface::handleScreenGeometryChange(screen(),
                                                           newData.geometry, newData.availableGeometry);
    }
}

HMONITOR QWindowsScreen::handle() const
{
    return m_data.hMonitor;
}

QRect QWindowsScreen::virtualGeometry(const QPlatformScreen *screen) // cf QScreen::virtualGeometry()
{
    QRect result;
    const auto siblings = screen->virtualSiblings();
    for (const QPlatformScreen *sibling : siblings)
        result |= sibling->geometry();
    return result;
}

bool QWindowsScreen::setOrientationPreference(Qt::ScreenOrientation o)
{
    bool result = false;
    ORIENTATION_PREFERENCE orientationPreference = ORIENTATION_PREFERENCE_NONE;
    switch (o) {
    case Qt::PrimaryOrientation:
        break;
    case Qt::PortraitOrientation:
        orientationPreference = ORIENTATION_PREFERENCE_PORTRAIT;
        break;
    case Qt::LandscapeOrientation:
        orientationPreference = ORIENTATION_PREFERENCE_LANDSCAPE;
        break;
    case Qt::InvertedPortraitOrientation:
        orientationPreference = ORIENTATION_PREFERENCE_PORTRAIT_FLIPPED;
        break;
    case Qt::InvertedLandscapeOrientation:
        orientationPreference = ORIENTATION_PREFERENCE_LANDSCAPE_FLIPPED;
        break;
    }
    result = SetDisplayAutoRotationPreferences(orientationPreference);
    return result;
}

Qt::ScreenOrientation QWindowsScreen::orientationPreference()
{
    Qt::ScreenOrientation result = Qt::PrimaryOrientation;
    ORIENTATION_PREFERENCE orientationPreference = ORIENTATION_PREFERENCE_NONE;
    if (GetDisplayAutoRotationPreferences(&orientationPreference)) {
        switch (orientationPreference) {
        case ORIENTATION_PREFERENCE_NONE:
            break;
        case ORIENTATION_PREFERENCE_LANDSCAPE:
            result = Qt::LandscapeOrientation;
            break;
        case ORIENTATION_PREFERENCE_PORTRAIT:
            result = Qt::PortraitOrientation;
            break;
        case ORIENTATION_PREFERENCE_LANDSCAPE_FLIPPED:
            result = Qt::InvertedLandscapeOrientation;
            break;
        case ORIENTATION_PREFERENCE_PORTRAIT_FLIPPED:
            result = Qt::InvertedPortraitOrientation;
            break;
        }
    }
    return result;
}

/*!
    \brief Queries ClearType settings to check the pixel layout
*/
QPlatformScreen::SubpixelAntialiasingType QWindowsScreen::subpixelAntialiasingTypeHint() const
{
    QPlatformScreen::SubpixelAntialiasingType type = QPlatformScreen::subpixelAntialiasingTypeHint();
    if (type == QPlatformScreen::Subpixel_None) {
        QSettings settings(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Avalon.Graphics\DISPLAY1)"_L1,
                           QSettings::NativeFormat);
        int registryValue = settings.value("PixelStructure"_L1, -1).toInt();
        switch (registryValue) {
        case 0:
            type = QPlatformScreen::Subpixel_None;
            break;
        case 1:
            type = QPlatformScreen::Subpixel_RGB;
            break;
        case 2:
            type = QPlatformScreen::Subpixel_BGR;
            break;
        default:
            type = QPlatformScreen::Subpixel_None;
            break;
        }
    }
    return type;
}

/*!
    \class QWindowsScreenManager
    \brief Manages a list of QWindowsScreen.

    Listens for changes and notifies QWindowSystemInterface about changed/
    added/deleted screens.

    \sa QWindowsScreen
    \internal
*/

extern "C" LRESULT QT_WIN_CALLBACK qDisplayChangeObserverWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DISPLAYCHANGE) {
        qCDebug(lcQpaScreen) << "Handling WM_DISPLAYCHANGE";
        if (QWindowsTheme *t = QWindowsTheme::instance())
            t->displayChanged();
        QWindowsWindow::displayChanged();
        QWindowsContext::instance()->screenManager().handleScreenChanges();
    }

    return DefWindowProc(hwnd, message, wParam, lParam);
}

QWindowsScreenManager::QWindowsScreenManager() = default;

void QWindowsScreenManager::initialize()
{
    qCDebug(lcQpaScreen) << "Initializing screen manager";

    auto className = QWindowsContext::instance()->registerWindowClass(
        QWindowsContext::classNamePrefix() + QLatin1String("ScreenChangeObserverWindow"),
        qDisplayChangeObserverWndProc);

    // HWND_MESSAGE windows do not get WM_DISPLAYCHANGE, so we need to create
    // a real top level window that we never show.
    m_displayChangeObserver = CreateWindowEx(0, reinterpret_cast<LPCWSTR>(className.utf16()),
        nullptr, WS_TILED, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    Q_ASSERT(m_displayChangeObserver);

    qCDebug(lcQpaScreen) << "Created display change observer" << m_displayChangeObserver;

    handleScreenChanges();
}

QWindowsScreenManager::~QWindowsScreenManager()
{
    DestroyWindow(m_displayChangeObserver);
}

bool QWindowsScreenManager::isSingleScreen()
{
    return QWindowsContext::instance()->screenManager().screens().size() < 2;
}

static inline int indexOfMonitor(const QWindowsScreenManager::WindowsScreenList &screens,
                                 const QString &deviceName)
{
    for (int i= 0; i < screens.size(); ++i)
        if (screens.at(i)->data().deviceName == deviceName)
            return i;
    return -1;
}

static inline int indexOfMonitor(const WindowsScreenDataList &screenData,
                                 const QString &deviceName)
{
    for (int i = 0; i < screenData.size(); ++i)
        if (screenData.at(i).deviceName == deviceName)
            return i;
    return -1;
}

// Move a window to a new virtual screen, accounting for varying sizes.
static void moveToVirtualScreen(QWindow *w, const QScreen *newScreen)
{
    QRect geometry = w->geometry();
    const QRect oldScreenGeometry = w->screen()->geometry();
    const QRect newScreenGeometry = newScreen->geometry();
    QPoint relativePosition = geometry.topLeft() - oldScreenGeometry.topLeft();
    if (oldScreenGeometry.size() != newScreenGeometry.size()) {
        const qreal factor =
            qreal(QPoint(newScreenGeometry.width(), newScreenGeometry.height()).manhattanLength()) /
            qreal(QPoint(oldScreenGeometry.width(), oldScreenGeometry.height()).manhattanLength());
        relativePosition = (QPointF(relativePosition) * factor).toPoint();
    }
    geometry.moveTopLeft(relativePosition);
    w->setGeometry(geometry);
}

void QWindowsScreenManager::removeScreen(int index)
{
    qCDebug(lcQpaScreen) << "Removing Monitor:" << m_screens.at(index)->data();
    QScreen *screen = m_screens.at(index)->screen();
    QScreen *primaryScreen = QGuiApplication::primaryScreen();
    // QTBUG-38650: When a screen is disconnected, Windows will automatically
    // move the Window to another screen. This will trigger a geometry change
    // event, but unfortunately after the screen destruction signal. To prevent
    // QtGui from automatically hiding the QWindow, pretend all Windows move to
    // the primary screen first (which is likely the correct, final screen).
    // QTBUG-39320: Windows does not automatically move WS_EX_TOOLWINDOW (dock) windows;
    // move those manually.
    if (screen != primaryScreen) {
        unsigned movedWindowCount = 0;
        const QWindowList tlws = QGuiApplication::topLevelWindows();
        for (QWindow *w : tlws) {
            if (w->screen() == screen && w->handle() && w->type() != Qt::Desktop) {
                if (w->isVisible() && w->windowState() != Qt::WindowMinimized
                    && (QWindowsWindow::baseWindowOf(w)->exStyle() & WS_EX_TOOLWINDOW)) {
                    moveToVirtualScreen(w, primaryScreen);
                } else {
                    QWindowSystemInterface::handleWindowScreenChanged(w, primaryScreen);
                }
                ++movedWindowCount;
            }
        }
        if (movedWindowCount)
            QWindowSystemInterface::flushWindowSystemEvents();
    }
    QWindowSystemInterface::handleScreenRemoved(m_screens.takeAt(index));
}

/*!
    \brief Synchronizes the screen list, adds new screens, removes deleted
    ones and propagates resolution changes to QWindowSystemInterface.
*/

bool QWindowsScreenManager::handleScreenChanges()
{
    // Look for changed monitors, add new ones
    const WindowsScreenDataList newDataList = monitorData();
    const bool lockScreen = newDataList.size() == 1 && (newDataList.front().flags & QWindowsScreenData::LockScreen);
    bool primaryScreenChanged = false;
    for (const QWindowsScreenData &newData : newDataList) {
        const int existingIndex = indexOfMonitor(m_screens, newData.deviceName);
        if (existingIndex != -1) {
            m_screens.at(existingIndex)->handleChanges(newData);
            if (existingIndex == 0)
                primaryScreenChanged = true;
        } else {
            auto *newScreen = new QWindowsScreen(newData);
            m_screens.push_back(newScreen);
            QWindowSystemInterface::handleScreenAdded(newScreen,
                                                             newData.flags & QWindowsScreenData::PrimaryScreen);
            qCDebug(lcQpaScreen) << "New Monitor: " << newData;
        }    // exists
    }        // for new screens.
    // Remove deleted ones but keep main monitors if we get only the
    // temporary lock screen to avoid window recreation (QTBUG-33062).
    if (!lockScreen) {
        for (int i = m_screens.size() - 1; i >= 0; --i) {
            if (indexOfMonitor(newDataList, m_screens.at(i)->data().deviceName) == -1)
                removeScreen(i);
        }     // for existing screens
    }     // not lock screen
    if (primaryScreenChanged) {
        if (auto theme = QWindowsTheme::instance()) // QTBUG-85734/Wine
            theme->refreshFonts();
    }
    return true;
}

void QWindowsScreenManager::clearScreens()
{
    // Delete screens in reverse order to avoid crash in case of multiple screens
    while (!m_screens.isEmpty())
        QWindowSystemInterface::handleScreenRemoved(m_screens.takeLast());
}

const QWindowsScreen *QWindowsScreenManager::screenAtDp(const QPoint &p) const
{
    for (QWindowsScreen *scr : m_screens) {
        if (scr->geometry().contains(p))
            return scr;
    }
    return nullptr;
}

const QWindowsScreen *QWindowsScreenManager::screenForHwnd(HWND hwnd) const
{
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
    if (hMonitor == nullptr)
        return nullptr;
    const auto it =
        std::find_if(m_screens.cbegin(), m_screens.cend(),
                     [hMonitor](const QWindowsScreen *s)
                     {
                         return s->data().hMonitor == hMonitor
                             && (s->data().flags & QWindowsScreenData::VirtualDesktop) != 0;
                     });
    return it != m_screens.cend() ? *it : nullptr;
}

QT_END_NAMESPACE
