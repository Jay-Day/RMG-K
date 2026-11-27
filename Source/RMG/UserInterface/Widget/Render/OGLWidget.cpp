/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "OGLWidget.hpp"

#include <QGuiApplication>

#include <RMG-Core/Callback.hpp>
#include <RMG-Core/Video.hpp>

#include <string>

using namespace UserInterface::Widget;

static std::string describeFormat(const QSurfaceFormat& format)
{
    const bool gles = format.renderableType() == QSurfaceFormat::OpenGLES;
    std::string text = gles ? "OpenGL ES " : "OpenGL ";
    text += std::to_string(format.majorVersion()) + "." + std::to_string(format.minorVersion());
    if (!gles && format.profile() == QSurfaceFormat::CompatibilityProfile)
    {
        text += " compatibility profile";
    }
    else if (!gles && format.profile() == QSurfaceFormat::CoreProfile)
    {
        text += " core profile";
    }
    return text;
}

OGLWidget::OGLWidget(QWidget *parent)
{
    // create window container
    this->widgetContainer = QWidget::createWindowContainer(this, parent);

    // on wayland we have to make sure that the widget
    // has a black background palette set, else
    // the window will have the theme as background color
    if (QGuiApplication::platformName() == "wayland")
    {
        QPalette blackPalette;
        blackPalette.setColor(QPalette::Window, Qt::black);
        this->widgetContainer->setAutoFillBackground(true); 
        this->widgetContainer->setPalette(blackPalette);
    }

    this->setSurfaceType(QWindow::OpenGLSurface);
    this->openGLcontext = new QOpenGLContext();
}

OGLWidget::~OGLWidget(void)
{
    this->openGLcontext->deleteLater();
}

void OGLWidget::MoveContextToThread(QThread* thread, const QSurfaceFormat* retryFormat)
{
    QOpenGLContext* context = this->GetContext();
    context->doneCurrent();
    if (!context->create() && retryFormat != nullptr)
    {
        // The context asks for the default format (no profile). Before giving
        // up, ask for exactly what the plugin set for the surface instead.
        CoreAddCallbackMessage(CoreDebugMessageType::Warning,
            "Couldn't create an OpenGL context with " + describeFormat(context->format()) +
            ", retrying with " + describeFormat(*retryFormat));
        context->setFormat(*retryFormat);
        context->create();
    }
    context->moveToThread(thread);
}

QOpenGLContext* OGLWidget::GetContext()
{
    return this->openGLcontext;
}

void OGLWidget::SetHideCursor(bool hide)
{
    this->setCursor(hide ? Qt::BlankCursor : Qt::ArrowCursor);
}

QWidget* OGLWidget::GetWidget(void)
{
    return this->widgetContainer;
}

void OGLWidget::resizeEvent(QResizeEvent *event)
{
    if (!this->isVisible())
    {
        return;
    }

    if (this->timerId != 0)
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }

    this->timerId = this->startTimer(100);

    // account for HiDPI scaling
    // see https://github.com/Rosalie241/RMG/issues/2
    this->width  = event->size().width() * this->devicePixelRatio();
    this->height = event->size().height() * this->devicePixelRatio();
}

void OGLWidget::timerEvent(QTimerEvent *event)
{
    if (!this->isVisible())
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
        return;
    }

    // only remove current timer
    // when setting the video size succeeds
    if (CoreSetVideoSize(this->width, this->height))
    {
        this->killTimer(this->timerId);
        this->timerId = 0;
    }
}
