#include "VltPackageArchive.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <array>
#include <memory>

namespace ui::vltarchive {
bool safeEntryPath(const QString& path) {
    if (path.isEmpty() || path.size() > 512 || QDir::isAbsolutePath(path)
        || path.contains('\\') || path.contains(':') || path.contains(QChar::Null)) return false;
    const auto parts = path.split('/');
    if (parts.size() > 32) return false;
    for (const auto& part : parts) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) return false;
        const auto stem = part.section('.', 0, 0).toUpper();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
            || (stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && stem[3] >= '1' && stem[3] <= '9')) return false;
        for (QChar c : part) if (c.unicode() < 32 || QStringLiteral("<>\"|?*").contains(c)) return false;
    }
    return true;
}
bool extract(const QString& path, const QString& destination, QString& error, KeepGoing keepGoing) {
    error.clear();
    const auto fail = [&](const QString& message) {error = message; return false;};
    QDir dir(destination);
    if (!dir.exists() || !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty()) return fail("Archive staging directory is not empty");
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(reader.get());
    archive_read_support_filter_none(reader.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), 64 * 1024) != ARCHIVE_OK) return fail("Unreadable ZIP package");
    archive_entry* entry = nullptr;
    int count = 0, status = ARCHIVE_OK;
    qint64 total = 0;
    QSet<QString> files;
    std::array<char, 64 * 1024> buffer;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        if (keepGoing && !keepGoing()) return fail("Installation cancelled");
        if (++count > kMaximumEntries) return fail("ZIP contains too many files");
        const char* utf8 = archive_entry_pathname_utf8(entry);
        if (!utf8) return fail("ZIP path is not UTF-8");
        QString relative = QString::fromUtf8(utf8);
        if (relative.endsWith('/')) relative.chop(1);
        if (!safeEntryPath(relative) || archive_entry_symlink(entry) || archive_entry_hardlink(entry)
            || archive_entry_is_encrypted(entry) > 0) return fail("ZIP contains an unsafe path, link or encrypted entry");
        const auto type = archive_entry_filetype(entry);
        const QString target = dir.filePath(relative);
        if (type == AE_IFDIR) {
            if (!QDir().mkpath(target)) return fail("Could not create package directory");
            continue;
        }
        if (type != AE_IFREG || files.contains(relative.toCaseFolded()) || QFileInfo::exists(target)) return fail("ZIP contains duplicate or unsupported entries");
        files.insert(relative.toCaseFolded());
        const qint64 expected = archive_entry_size(entry);
        if (expected < 0 || expected > 1'000'000'000LL || expected > kMaximumExpandedBytes - total) return fail("ZIP exceeds the expanded size limit");
        if (QStorageInfo(destination).bytesAvailable() < expected + 1'000'000) return fail("Not enough free disk space");
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) return fail("Could not create package directory");
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) return fail(output.errorString());
        qint64 written = 0;
        la_ssize_t bytes;
        while ((bytes = archive_read_data(reader.get(), buffer.data(), buffer.size())) > 0) {
            if (keepGoing && !keepGoing()) return fail("Installation cancelled");
            if (bytes > expected - written || output.write(buffer.data(), bytes) != bytes) return fail("Incomplete package or insufficient disk space");
            written += bytes;
        }
        if (bytes < 0 || written != expected || !output.commit()) return fail("Damaged ZIP package");
        QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        total += written;
    }
    if (status != ARCHIVE_EOF || count == 0) return fail("Damaged or empty ZIP package");
    return true;
}
namespace {
la_ssize_t writeArchive(archive*, void* data, const void* bytes, size_t size) {
    auto* output=static_cast<QSaveFile*>(data);
    if(qint64(size)>1'000'000'000LL-output->pos())return -1;
    return output->write(static_cast<const char*>(bytes), qint64(size));
}
}
bool pack(const QString& directory, const QString& destination, QString& error, KeepGoing keepGoing) {
    error.clear();
    const auto fail = [&](const QString& message) {error = message; return false;};
    const QDir root(directory);
    if (!root.exists() || QFileInfo(directory).isSymLink()) return fail("Choose a saved project or template directory");
    const QString absoluteTarget = QFileInfo(destination).absoluteFilePath();
    if (absoluteTarget.startsWith(root.absolutePath() + '/')) return fail("Save the ZIP outside the source project");
    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)) return fail(output.errorString());
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    archive_write_set_format_zip(writer.get());
    archive_write_set_options(writer.get(), "zip:compression=deflate");
    if (archive_write_open(writer.get(), &output, nullptr, writeArchive, nullptr) != ARCHIVE_OK) return fail("Could not start ZIP export");
    QDirIterator it(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    int count = 0; qint64 total = 0;
    QSet<QString> names;
    std::array<char, 64 * 1024> buffer;
    while (it.hasNext()) {
        it.next(); const auto info = it.fileInfo();
        if (keepGoing && !keepGoing()) return fail("Export cancelled");
        if (++count > kMaximumEntries || info.isSymLink()) return fail("Project contains links or too many files");
        const auto name = root.relativeFilePath(info.absoluteFilePath());
        if (!safeEntryPath(name) || names.contains(name.toCaseFolded())) return fail("Project contains a non-portable filename");
        names.insert(name.toCaseFolded());
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname_utf8(entry.get(), name.toUtf8().constData());
        archive_entry_set_perm(entry.get(), info.isDir() ? 0700 : 0600);
        archive_entry_set_filetype(entry.get(), info.isDir() ? AE_IFDIR : AE_IFREG);
        archive_entry_set_size(entry.get(), info.isDir() ? 0 : info.size());
        if (!info.isDir() && (!info.isFile() || info.size() > 1'000'000'000LL || info.size() > kMaximumExpandedBytes-total)) return fail("Project exceeds the portable package limit");
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK) return fail("Could not write ZIP header");
        if (info.isDir()) continue;
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) return fail(input.errorString());
        qint64 copied = 0, n;
        while ((n = input.read(buffer.data(), buffer.size())) > 0) {
            if (keepGoing && !keepGoing()) return fail("Export cancelled");
            if (n > info.size()-copied || archive_write_data(writer.get(), buffer.data(), size_t(n)) != n) return fail("Could not write ZIP data; check disk space and the 1 GB package limit");
            copied += n;
        }
        if (n < 0 || copied != info.size()) return fail("Source file changed during export");
        total += copied;
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK || !output.commit()) return fail("Could not finish ZIP export");
    return true;
}
}
