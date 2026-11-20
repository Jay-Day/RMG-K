/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef EVENTFILTER_HPP
#define EVENTFILTER_HPP

#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QList>
#include <QObject>

namespace UserInterface
{
class EventFilter : public QObject
{
    Q_OBJECT

  public:
    EventFilter(QObject *);
    ~EventFilter(void);

  protected:
    bool eventFilter(QObject *, QEvent *);

  signals:
    void on_EventFilter_KeyPressed(QKeyEvent *);
    void on_EventFilter_KeyReleased(QKeyEvent *);

    void on_EventFilter_FileDropped(QDropEvent *);
};

// Takes one window's input method events while enabled, which should only be while
// text is being typed: at any other time an input method would swallow keys meant
// for the game. Key events bubble up to the window, but these only go to the focused
// object, which can be any child of it, so this filters the whole application.
class InputMethodFilter : public QObject
{
    Q_OBJECT

  public:
    InputMethodFilter(QWidget *window);

    void SetEnabled(bool enabled);

  protected:
    bool eventFilter(QObject *, QEvent *);

  private:
    bool isInWindow(QObject *object) const;

    QWidget *window = nullptr;
    bool enabled    = false;

  signals:
    void on_InputMethodFilter_Query(QInputMethodQueryEvent *);
    void on_InputMethodFilter_Event(QInputMethodEvent *);
};
} // namespace UserInterface

#endif // EVENTFILTER_HPP