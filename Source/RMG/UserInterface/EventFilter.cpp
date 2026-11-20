/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "EventFilter.hpp"

#include <QCoreApplication>
#include <QWidget>
#include <QWindow>

using namespace UserInterface;

EventFilter::EventFilter(QObject *parent) : QObject(parent)
{
}

EventFilter::~EventFilter(void)
{
}

bool EventFilter::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type())
    {
    case QEvent::Type::KeyPress:
        emit this->on_EventFilter_KeyPressed(static_cast<QKeyEvent*>(event));
        return true;
    case QEvent::Type::Drop:
        emit this->on_EventFilter_FileDropped(static_cast<QDropEvent*>(event));
        return true;
    case QEvent::Type::KeyRelease:
        emit this->on_EventFilter_KeyReleased(static_cast<QKeyEvent*>(event));
        return true;
    default:
        break;
    }

    return QObject::eventFilter(object, event);
}

InputMethodFilter::InputMethodFilter(QWidget *window) : QObject(window), window(window)
{
}

void InputMethodFilter::SetEnabled(bool enabled)
{
    if (enabled == this->enabled)
    {
        return;
    }

    this->enabled = enabled;
    if (enabled)
    {
        QCoreApplication::instance()->installEventFilter(this);
    }
    else
    {
        QCoreApplication::instance()->removeEventFilter(this);
    }
}

bool InputMethodFilter::isInWindow(QObject *object) const
{
    if (const QWidget *widget = qobject_cast<QWidget*>(object))
    {
        return widget->window() == this->window;
    }

    if (const QWindow *nativeWindow = qobject_cast<QWindow*>(object))
    {
        // the render surface is a child window of the main one
        while (nativeWindow->parent() != nullptr)
        {
            nativeWindow = nativeWindow->parent();
        }
        return nativeWindow == this->window->windowHandle();
    }

    return false;
}

bool InputMethodFilter::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::Type::InputMethodQuery && this->isInWindow(object))
    {
        emit this->on_InputMethodFilter_Query(static_cast<QInputMethodQueryEvent*>(event));
        return true;
    }

    if (event->type() == QEvent::Type::InputMethod && this->isInWindow(object))
    {
        emit this->on_InputMethodFilter_Event(static_cast<QInputMethodEvent*>(event));
        return true;
    }

    return QObject::eventFilter(object, event);
}
