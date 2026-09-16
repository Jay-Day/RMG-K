/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "InstallUpdateDialog.hpp"
#include "Utilities/QtMessageBox.hpp"

#include <QCoreApplication>
#include <QDirIterator>
#include <QTextStream>
#include <QTimerEvent>
#include <QDateTime>
#include <QFileInfo>
#include <QFile>
#include <QDir>

#include <RMG-Core/Directories.hpp>
#include <RMG-Core/Archive.hpp>
#include <RMG-Core/Error.hpp>

using namespace UserInterface::Dialog;
using namespace Utilities;

//
// Local Functions
//

static QString get_cleanup_list_path(void)
{
    return QString::fromStdU32String(CoreGetUserCacheDirectory().u32string()) + "/UpdateCleanup.txt";
}

//
// Exported Functions
//

InstallUpdateDialog::InstallUpdateDialog(QWidget *parent, QString installationDirectory, QString temporaryDirectory, QString filename) : QDialog(parent)
{
    this->setupUi(this);

    this->installationDirectory = installationDirectory;
    this->temporaryDirectory = temporaryDirectory;
    this->filename = filename;
    this->startTimer(100);
}

InstallUpdateDialog::~InstallUpdateDialog(void)
{
}

QString InstallUpdateDialog::GetLaunchProgram(void)
{
    return this->launchProgram;
}

QStringList InstallUpdateDialog::GetLaunchArguments(void)
{
    return this->launchArguments;
}

QString InstallUpdateDialog::GetUpdateDirectory(void)
{
    return QString::fromStdU32String(CoreGetUserCacheDirectory().u32string()) + "/Update";
}

QString InstallUpdateDialog::GetLogPath(void)
{
    return QString::fromStdU32String(CoreGetUserCacheDirectory().u32string()) + "/updater.log";
}

void InstallUpdateDialog::WriteLog(QString logPath, QString message)
{
    QFile logFile(logPath);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream textStream(&logFile);
    textStream << QDateTime::currentDateTime().toString(Qt::ISODate) << " == " << message << "\n";
    logFile.close();
}

void InstallUpdateDialog::CleanupPreviousUpdate(void)
{
    const QString appPath = QDir::cleanPath(QCoreApplication::applicationDirPath());

    // remove the files which were replaced by the previous update,
    // only accept '.old' files inside of the application directory
    QFile cleanupListFile(get_cleanup_list_path());
    if (cleanupListFile.exists())
    {
        bool removedAllFiles = true;

        if (cleanupListFile.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            QTextStream textStream(&cleanupListFile);
            while (!textStream.atEnd())
            {
                const QString filePath = QDir::cleanPath(textStream.readLine().trimmed());
                if (filePath.isEmpty() ||
                    !filePath.contains(".old") ||
                    !filePath.startsWith(appPath + "/", Qt::CaseInsensitive))
                {
                    continue;
                }

                if (QFile::exists(filePath) && !QFile::remove(filePath))
                {
                    WriteLog(GetLogPath(), "Failed to remove '" + filePath + "', retrying on next start");
                    removedAllFiles = false;
                }
            }
            cleanupListFile.close();
        }

        if (removedAllFiles)
        {
            cleanupListFile.remove();
        }
    }

    // remove the downloaded update files,
    // this fails silently when an installer is still running
    QDir updateDirectory(GetUpdateDirectory());
    if (updateDirectory.exists())
    {
        updateDirectory.removeRecursively();
    }
}

void InstallUpdateDialog::install(void)
{
    const QString fullFilePath = QDir(this->temporaryDirectory).filePath(this->filename);
    const QString logPath      = GetLogPath();

    // start with a fresh log for every update attempt
    QFile::remove(logPath);
    WriteLog(logPath, "Installing '" + fullFilePath + "' into '" + this->installationDirectory + "'");

    if (this->filename.endsWith(".exe", Qt::CaseInsensitive))
    {
        // the installer is started by RMG-K after it has shut down,
        // it then replaces the installed files and relaunches RMG-K
        this->label->setText("Preparing " + this->filename + "...");
        this->progressBar->setValue(100);

        // the installer writes its own log next to ours, it overwrites
        // any existing file so it can't share updater.log
        const QString setupLogPath = QFileInfo(logPath).dir().filePath("updater-setup.log");

        this->launchProgram   = QDir::toNativeSeparators(fullFilePath);
        this->launchArguments =
        {
            "/SILENT",
            "/NOCANCEL",
            "/CLOSEAPPLICATIONS",
            "/MERGETASKS=!desktopicon",
            "/DIR=" + QDir::toNativeSeparators(this->installationDirectory),
            "/LOG=" + QDir::toNativeSeparators(setupLogPath),
        };
        WriteLog(logPath, "Installer will be started after exit, see '" + setupLogPath + "' for its output");
        this->accept();
        return;
    }

    this->label->setText("Extracting " + this->filename + "...");
    this->progressBar->setValue(25);

    QDir dir(this->temporaryDirectory);
    if (!dir.mkdir("extract"))
    {
        WriteLog(logPath, "Failed to create extract directory in '" + this->temporaryDirectory + "'");
        QtMessageBox::Error(this, "QDir::mkdir() Failed", "");
        this->reject();
        return;
    }

    const QString extractDirectory = dir.filePath("extract");

    if (!CoreUnzip(fullFilePath.toStdU32String(), extractDirectory.toStdU32String()))
    {
        const QString coreError = QString::fromStdString(CoreGetError());
        WriteLog(logPath, "Failed to extract '" + fullFilePath + "': " + coreError);
        QtMessageBox::Error(this, "CoreUnzip() Failed", coreError);
        this->reject();
        return;
    }

    this->label->setText("Installing update...");
    this->progressBar->setValue(50);

    QString error;
    if (!this->installPortable(extractDirectory, error))
    {
        WriteLog(logPath, "Failed to install update: " + error);
        QtMessageBox::Error(this, "Failed to install update", error + ", check " + QDir::toNativeSeparators(logPath) + " for more information");
        this->reject();
        return;
    }

    this->progressBar->setValue(100);

    // we don't need the downloaded files anymore
    QDir(this->temporaryDirectory).removeRecursively();

    this->launchProgram = QDir::toNativeSeparators(QDir(this->installationDirectory).filePath(QFileInfo(QCoreApplication::applicationFilePath()).fileName()));
    this->launchArguments.clear();
    WriteLog(logPath, "Update installed, '" + this->launchProgram + "' will be started after exit");
    this->accept();
}

bool InstallUpdateDialog::installPortable(QString extractDirectory, QString& error)
{
    struct ReplacedFile
    {
        QString path;
        QString oldPath;
    };

    QDir sourceDir(extractDirectory);
    QDir targetDir(this->installationDirectory);
    QList<ReplacedFile> replacedFiles;
    const QString logPath = GetLogPath();

    // restores the installation to its previous state
    auto rollback = [&]()
    {
        for (auto it = replacedFiles.rbegin(); it != replacedFiles.rend(); ++it)
        {
            if (QFile::exists(it->path) && !QFile::remove(it->path))
            {
                WriteLog(logPath, "Rollback: failed to remove '" + it->path + "'");
            }
            if (!it->oldPath.isEmpty() && !QFile::rename(it->oldPath, it->path))
            {
                WriteLog(logPath, "Rollback: failed to restore '" + it->path + "' from '" + it->oldPath + "'");
            }
        }
    };

    QDirIterator iterator(extractDirectory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext())
    {
        const QString sourcePath   = iterator.next();
        const QString relativePath = sourceDir.relativeFilePath(sourcePath);
        const QString targetPath   = targetDir.filePath(relativePath);

        if (!targetDir.mkpath(QFileInfo(targetPath).path()))
        {
            error = "Failed to create directory for " + relativePath;
            rollback();
            return false;
        }

        ReplacedFile replacedFile = { targetPath, "" };

        // files which are in use (i.e the executable and loaded libraries)
        // can't be overwritten or removed but they can be renamed,
        // so we move every existing file out of the way and keep it
        // around until the update has succeeded
        if (QFile::exists(targetPath))
        {
            QString oldPath = targetPath + ".old";
            for (int i = 1; QFile::exists(oldPath) && !QFile::remove(oldPath); i++)
            {
                oldPath = targetPath + ".old" + QString::number(i);
            }

            if (!QFile::rename(targetPath, oldPath))
            {
                error = "Failed to rename " + relativePath;
                rollback();
                return false;
            }

            replacedFile.oldPath = oldPath;
        }

        replacedFiles.append(replacedFile);

        if (!QFile::copy(sourcePath, targetPath))
        {
            error = "Failed to copy " + relativePath;
            rollback();
            return false;
        }
    }

    // store which old files have to be removed
    // when the updated version of RMG-K starts
    QFile cleanupListFile(get_cleanup_list_path());
    if (cleanupListFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        QTextStream textStream(&cleanupListFile);
        for (const ReplacedFile& replacedFile : replacedFiles)
        {
            if (replacedFile.oldPath.isEmpty())
            {
                continue;
            }

            // remove the old file right away when it isn't in use
            if (!QFile::remove(replacedFile.oldPath))
            {
                WriteLog(logPath, "'" + replacedFile.oldPath + "' is in use, removing on next start");
                textStream << QDir::cleanPath(replacedFile.oldPath) << "\n";
            }
        }
        cleanupListFile.close();
    }
    else
    {
        WriteLog(logPath, "Failed to open '" + cleanupListFile.fileName() + "', old files won't be removed");
    }

    WriteLog(logPath, "Replaced " + QString::number(replacedFiles.size()) + " files");
    return true;
}

void InstallUpdateDialog::timerEvent(QTimerEvent *event)
{
    this->killTimer(event->timerId());
    this->install();
}
