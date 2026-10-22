/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 */
#ifndef CREATEROOMDIALOG_HPP
#define CREATEROOMDIALOG_HPP

#ifdef NETPLAY

#include <QDialog>
#include <QString>
#include <QList>
#include <QPair>
#include <QMap>

class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;

namespace UserInterface
{
namespace Dialog
{

// What the lobby knows about ranked play for the selected game.
struct CreateRoomRankedOptions
{
    bool available = false;
    // Why ranked can't be chosen (shown when !available); empty hides the
    // ranked section entirely (server without ranked support).
    QString unavailableReason;
    // one per ladder the game is on: 1v1 and, when the server has one, 2v2
    struct Format
    {
        QString ladderId;
        QString ladderName;
        int teamSize = 1;
        QList<int> bestOf; // allowed set lengths, first = default
        // allowed rulesets (e.g. rosters) as id + display name, first = default
        QList<QPair<QString, QString>> rulesets;
        // the player's standing per roster, e.g. "✓ Gold 1532"
        QMap<QString, QString> standings;
    };
    QList<Format> formats;
};

// Modal form for creating a lobby room. Returns the chosen settings to the
// caller via accessors; caller is responsible for sending the protocol message.
class CreateRoomDialog : public QDialog
{
    Q_OBJECT

public:
    // The game is chosen in the lobby's shared picker and passed in here; this
    // dialog shows it read-only and reports it back via romName()/romMd5().
    CreateRoomDialog(const QString& defaultUsername, const QString& defaultRoomName,
                     const QString& gameName, const QString& gameMd5,
                     const CreateRoomRankedOptions& ranked = CreateRoomRankedOptions(),
                     QWidget* parent = nullptr);
    ~CreateRoomDialog() override = default;

    // Accessors valid only after exec() returns Accepted.
    QString name()       const { return m_name; }
    QString romName()    const { return m_romName; }
    QString romMd5()     const { return m_romMd5; }
    QString romRegion()  const { return m_romRegion; }
    int     maxPlayers() const { return m_maxPlayers; }
    int     delay()      const { return m_delay; }
    int     prediction() const { return m_prediction; }
    QString password()   const { return m_password; }
    bool    ranked()           const { return m_isRanked; }
    int     bestOf()           const { return m_bestOf; }
    QString rulesetId()        const { return m_rulesetId; }
    QString ladderId()         const { return m_ladderId; }
    int teamSize()             const { return m_teamSize; }
    int     maxTierGap()       const { return m_maxTierGap; }
    int     maxPing()          const { return m_maxPing; }
    bool    allowProvisional() const { return m_allowProvisional; }

    // Called by the lobby when the server replies ROOM_CREATE_FAIL.
    // Re-enables the form so the user can edit and retry.
    void showCreateFailure(const QString& reason);

    // Display-name formatting, exposed so the lobby's browse ROM picker can
    // render identical game names (keeps the Create Room pre-select in sync).
    static QString displayGameName(const QString& goodName, const QString& filePath);

signals:
    // Emitted when user clicks Create with valid input. The dialog stays
    // open in a "Creating..." state until either showCreateFailure() is
    // called (re-enables form) or the caller calls accept().
    void createRequested();

private slots:
    void onCreateClicked();
    void onPasswordToggled(bool enabled);
    void onRankedToggled(bool enabled);
    // fills set length and roster for the chosen format
    void onFormatChanged();
    void validateInput();

private:
    void buildUi(const QString& defaultUsername, const QString& defaultRoomName);
    void loadDefaults();
    void saveDefaults();
    void setFormEnabled(bool enabled);

    // UI — local delay and prediction live in the room view.
    QLineEdit*   m_nameEdit       = nullptr;
    QLabel*      m_gameLabel      = nullptr;   // read-only game from the lobby picker
    QSpinBox*    m_maxPlayersSpin = nullptr;
    QCheckBox*   m_passwordCheck  = nullptr;
    QLineEdit*   m_passwordEdit   = nullptr;
    QPushButton* m_createButton   = nullptr;
    QPushButton* m_cancelButton   = nullptr;
    QLabel*      m_statusLabel    = nullptr;
    QCheckBox*   m_rankedCheck    = nullptr;
    QWidget*     m_rankedOptions  = nullptr;
    QComboBox*   m_bestOfCombo    = nullptr;
    QComboBox*   m_rulesetCombo   = nullptr;
    QComboBox*   m_formatCombo    = nullptr;
    QComboBox*   m_skillCombo     = nullptr;
    QComboBox*   m_pingCombo      = nullptr;
    QCheckBox*   m_provisionalCheck = nullptr;
    CreateRoomRankedOptions m_rankedInfo;

    // Accessor backing
    QString m_name;
    QString m_romName;
    QString m_romMd5;
    QString m_romRegion;
    int     m_maxPlayers = 2;
    int     m_delay      = 2;
    int     m_prediction = 7;
    QString m_password;
    bool    m_isRanked = false;
    int     m_bestOf = 0;
    QString m_rulesetId;
    QString m_ladderId;
    int m_teamSize = 1;
    int     m_maxTierGap = 0;
    int     m_maxPing = 120;
    bool    m_allowProvisional = true;
};

} // namespace Dialog
} // namespace UserInterface

#endif // NETPLAY
#endif // CREATEROOMDIALOG_HPP
