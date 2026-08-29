/*
 * Fooyin
 * Copyright © 2026, Luke Taylor <luket@pm.me>
 *
 * Fooyin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Fooyin is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Fooyin.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "core/library/libraryscanner.h"
#include "core/database/dbschema.h"
#include "core/database/trackdatabase.h"
#include "core/library/libraryscansession.h"
#include "core/library/libraryscanstate.h"
#include "core/library/libraryscanutils.h"
#include "core/library/libraryscanwriter.h"
#include "core/library/librarytrackresolver.h"
#include "core/playlist/parsers/cueparser.h"
#include "core/playlist/playlistloader.h"

#include <core/engine/audioloader.h>
#include <core/trackmetadatastore.h>
#include <utils/database/dbconnectionhandler.h>
#include <utils/database/dbconnectionpool.h>
#include <utils/database/dbconnectionprovider.h>
#include <utils/database/dbquery.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QLoggingCategory>
#include <QSqlError>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace {
QCoreApplication* ensureCoreApplication()
{
    QStandardPaths::setTestModeEnabled(true);
    QLoggingCategory::setFilterRules(u"fy.db.info=false"_s);

    if(auto* app = QCoreApplication::instance()) {
        return app;
    }

    static int argc{1};
    static char appName[]        = "fooyin-libraryscanner-test";
    static char* argv[]          = {appName, nullptr};
    static QCoreApplication* app = []() {
        auto* instance = new QCoreApplication(argc, argv);
        QCoreApplication::setApplicationName(QString::fromLatin1(appName));
        return instance;
    }();
    return app;
}

class DummyScanHost : public Fooyin::LibraryScanHost
{
public:
    [[nodiscard]] bool stopRequested() const override
    {
        return false;
    }

    void reportProgress(int /*current*/, const QString& /*file*/, int /*total*/, int /*phase*/,
                        int /*discovered*/) override
    { }
    void reportScanUpdate(const Fooyin::ScanResult& /*result*/) override { }
};

class FakeEmbeddedCueReader : public Fooyin::AudioReader
{
public:
    struct State
    {
        int readCalls{0};
        QString cueSheet;
        std::vector<std::pair<QString, QString>> extraTags;
    };

    explicit FakeEmbeddedCueReader(std::shared_ptr<State> state)
        : m_state{std::move(state)}
    { }

    QStringList extensions() const override
    {
        return {u"flac"_s, u"bin"_s};
    }

    bool canReadCover() const override
    {
        return false;
    }

    bool canWriteMetaData() const override
    {
        return false;
    }

    bool readTrack(const Fooyin::AudioSource&, Fooyin::Track& track) override
    {
        ++m_state->readCalls;
        track.setTitle(u"Embedded Album"_s);
        if(!m_state->cueSheet.isEmpty()) {
            track.replaceExtraTag(u"CUESHEET"_s, m_state->cueSheet);
        }
        for(const auto& [key, value] : m_state->extraTags) {
            track.replaceExtraTag(key, value);
        }
        return true;
    }

private:
    std::shared_ptr<State> m_state;
};

void writeFile(const QString& path, const QByteArray& data)
{
    QFile file{path};
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(data.size(), file.write(data));
}

constexpr auto CurrentSchemaVersion = 19;

Fooyin::DbConnectionPoolPtr createTestDbPool(const QString& dbPath)
{
    static std::atomic_int counter{0};

    Fooyin::DbConnection::DbParams params;
    params.type           = u"QSQLITE"_s;
    params.connectOptions = u"QSQLITE_OPEN_URI"_s;
    params.filePath       = dbPath;

    const auto connectionName = u"fooyin-libraryscanner-test-%1"_s.arg(counter.fetch_add(1));
    auto dbPool               = Fooyin::DbConnectionPool::create(params, connectionName);

    const Fooyin::DbConnectionHandler handler{dbPool};
    EXPECT_TRUE(handler.hasConnection());

    Fooyin::DbSchema schema{Fooyin::DbConnectionProvider{dbPool}};
    const auto upgradeResult = schema.upgradeDatabase(CurrentSchemaVersion, u"://dbschema.xml"_s);
    EXPECT_TRUE(upgradeResult == Fooyin::DbSchema::UpgradeResult::Success
                || upgradeResult == Fooyin::DbSchema::UpgradeResult::IsCurrent
                || upgradeResult == Fooyin::DbSchema::UpgradeResult::BackwardsCompatible);

    return dbPool;
}

//! Expands a cue sheet exactly as the scanner will, so seeded rows line up with reloaded tracks.
Fooyin::TrackList expandCueSheet(const QString& cueSheet, const QString& filepath)
{
    QByteArray bytes{cueSheet.toUtf8()};
    QBuffer buffer{&bytes};
    EXPECT_TRUE(buffer.open(QIODevice::ReadOnly | QIODevice::Text));

    Fooyin::PlaylistParser::ReadPlaylistEntry readEntry;
    readEntry.readTrack = [](const Fooyin::Track& track) {
        return track;
    };
    readEntry.canLoadTrack = [](const Fooyin::Track&) {
        return true;
    };

    Fooyin::CueParser parser;
    return parser.readPlaylist(&buffer, filepath, {}, readEntry, false);
}

void sortTracks(Fooyin::TrackList& tracks)
{
    std::ranges::sort(tracks, [](const Fooyin::Track& lhs, const Fooyin::Track& rhs) {
        if(lhs.filepath() != rhs.filepath()) {
            return lhs.filepath() < rhs.filepath();
        }
        if(lhs.offset() != rhs.offset()) {
            return lhs.offset() < rhs.offset();
        }
        return lhs.title() < rhs.title();
    });
}
} // namespace

namespace Fooyin::Testing {
namespace {
constexpr auto TwoTrackSheet = R"(FILE "album.flac" FLAC
  TRACK 01 AUDIO
    TITLE "Dixit Dominus"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Laudate pueri"
    INDEX 01 01:00:00
)";

struct ReloadFixture
{
    QTemporaryDir dir;
    QString audioPath;
    DbConnectionPoolPtr dbPool;
    // The pool hands out per-thread connections, so a handler must outlive every query below.
    std::unique_ptr<DbConnectionHandler> dbHandler;
    int libraryId{-1};
    std::shared_ptr<PlaylistLoader> playlistLoader{std::make_shared<PlaylistLoader>()};
    std::shared_ptr<AudioLoader> audioLoader{std::make_shared<AudioLoader>()};
    std::shared_ptr<TrackMetadataStore> metadataStore{std::make_shared<TrackMetadataStore>()};
    std::shared_ptr<FakeEmbeddedCueReader::State> readerState{std::make_shared<FakeEmbeddedCueReader::State>()};
    TrackDatabase trackDb;

    ReloadFixture()
    {
        audioPath = dir.filePath(u"album.flac"_s);
        writeFile(audioPath, "flac");

        dbPool    = createTestDbPool(dir.filePath(u"library.db"_s));
        dbHandler = std::make_unique<DbConnectionHandler>(dbPool);
        trackDb.initialise(DbConnectionProvider{dbPool});

        const DbConnectionProvider dbProvider{dbPool};
        DbQuery insertQuery{dbProvider.db(), u"INSERT INTO Libraries (Name, Path) VALUES (:name, :path);"_s};
        insertQuery.bindValue(u":name"_s, u"Test"_s);
        insertQuery.bindValue(u":path"_s, dir.path());
        EXPECT_TRUE(insertQuery.exec()) << insertQuery.lastError().text().toStdString();
        libraryId = insertQuery.lastInsertId().toInt();

        playlistLoader->addParser(std::make_unique<CueParser>());

        auto state = readerState;
        audioLoader->addReader(
            u"fake-embedded"_s, [state]() { return std::make_unique<FakeEmbeddedCueReader>(state); }, 0);
    }

    //! Writes the tracks a given cue sheet expands to into the database, as an earlier scan would have.
    TrackList seedTracks(const QString& cueSheet)
    {
        return storeTracks(expandCueSheet(cueSheet, audioPath));
    }

    TrackList storeTracks(TrackList tracks)
    {
        for(Track& track : tracks) {
            track.setMetadataStore(metadataStore);
            track.setLibraryId(libraryId);
            track.setIsEnabled(true);
            track.generateHash();
        }

        EXPECT_TRUE(trackDb.storeTracks(tracks));
        for(const Track& track : tracks) {
            EXPECT_TRUE(track.isInDatabase()) << "seeded track was not assigned an id";
        }

        return tracks;
    }
};
} // namespace

TEST(LibraryScannerTest, ReloadingEmbeddedCueTracksUpdatesRowsAndAppliesCueTrackTags)
{
    ensureCoreApplication();

    ReloadFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());

    fixture.readerState->cueSheet  = QString::fromUtf8(TwoTrackSheet);
    fixture.readerState->extraTags = {{u"Cue_track01_composer"_s, u"Handel"_s},
                                      {u"Cue_track02_composer"_s, u"Monteverdi"_s},
                                      {u"Cue_track01_conductor"_s, u"Gardiner"_s}};

    const TrackList seeded = fixture.seedTracks(QString::fromUtf8(TwoTrackSheet));
    ASSERT_EQ(2U, seeded.size());

    LibraryScanner scanner{fixture.dbPool, fixture.playlistLoader, fixture.metadataStore, fixture.audioLoader, nullptr};
    scanner.initialiseThread();

    ScanResult result;
    QObject::connect(&scanner, &LibraryScanner::scanUpdate, &scanner,
                     [&result](const ScanResult& update) { result = update; });

    scanner.scanTracks(seeded, false, {});

    ASSERT_EQ(2U, result.updatedTracks.size()) << "both cue tracks should have been reloaded";

    TrackList updated = result.updatedTracks;
    sortTracks(updated);

    // The existing rows are updated in place, not replaced.
    EXPECT_EQ(seeded.at(0).id(), updated.at(0).id());
    EXPECT_EQ(seeded.at(1).id(), updated.at(1).id());
    EXPECT_TRUE(result.addedTracks.empty());

    // Per-track tags reached the right track, and the parent's clutter did not.
    EXPECT_EQ(QStringList{u"Handel"_s}, updated.at(0).composers());
    EXPECT_EQ(QStringList{u"Monteverdi"_s}, updated.at(1).composers());
    EXPECT_EQ(QStringList{u"Gardiner"_s}, updated.at(0).extraTag(u"CONDUCTOR"_s));
    EXPECT_FALSE(updated.at(1).hasExtraTag(u"CUE_TRACK01_COMPOSER"_s));

    // Identity carried over from the seeded rows.
    EXPECT_EQ(fixture.libraryId, updated.at(0).libraryId());
    EXPECT_TRUE(updated.at(0).isEnabled());
}

// End-to-end guard for the matcher: a re-cut sheet must never produce two updates aimed at one
// database row, which would silently overwrite one track's metadata and strand another row.
TEST(LibraryScannerTest, ReloadingRenumberedCueSheetNeverWritesDuplicateIds)
{
    ensureCoreApplication();

    ReloadFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());

    fixture.readerState->cueSheet = QString::fromUtf8(TwoTrackSheet);

    // Seed a single row occupying exactly the segment the sheet's TRACK 01 will expand to, but
    // numbered 02. That is the shape that lets a track-number fallback re-claim a row an identity
    // match has already taken: reloaded TRACK 01 matches it physically, and reloaded TRACK 02 then
    // matches it by number. Distinct track numbers never collide, so a naive re-cut proves nothing.
    TrackList seedSource = expandCueSheet(QString::fromUtf8(TwoTrackSheet), fixture.audioPath);
    ASSERT_EQ(2U, seedSource.size());
    TrackList toSeed{seedSource.at(0)};
    toSeed[0].setTrackNumber(u"02"_s);

    const TrackList seeded = fixture.storeTracks(toSeed);
    ASSERT_EQ(1U, seeded.size());

    LibraryScanner scanner{fixture.dbPool, fixture.playlistLoader, fixture.metadataStore, fixture.audioLoader, nullptr};
    scanner.initialiseThread();

    ScanResult result;
    QObject::connect(&scanner, &LibraryScanner::scanUpdate, &scanner,
                     [&result](const ScanResult& update) { result = update; });

    scanner.scanTracks(seeded, false, {});

    std::set<int> seenIds;
    for(const Track& track : result.updatedTracks) {
        EXPECT_TRUE(track.isInDatabase()) << "an update was queued for a track with no database id";
        EXPECT_TRUE(seenIds.insert(track.id()).second)
            << "database row " << track.id() << " was targeted by more than one update";
    }

    // No update may target a row that was never seeded.
    std::set<int> seededIds;
    for(const Track& track : seeded) {
        seededIds.insert(track.id());
    }
    for(const Track& track : result.updatedTracks) {
        EXPECT_TRUE(seededIds.contains(track.id())) << "update targeted an unknown row";
    }

    EXPECT_LE(result.updatedTracks.size(), seeded.size());
}

TEST(LibraryScannerTest, ReloadingLeavesExternalCueTracksAlone)
{
    ensureCoreApplication();

    ReloadFixture fixture;
    ASSERT_TRUE(fixture.dir.isValid());

    fixture.readerState->cueSheet = QString::fromUtf8(TwoTrackSheet);

    TrackList seeded = fixture.seedTracks(QString::fromUtf8(TwoTrackSheet));
    ASSERT_EQ(2U, seeded.size());

    // Re-point them at an external cue file; their metadata comes from the .cue, not the audio file.
    const QString cuePath = fixture.dir.filePath(u"album.cue"_s);
    for(Track& track : seeded) {
        track.setCuePath(cuePath);
    }
    ASSERT_FALSE(seeded.at(0).hasEmbeddedCue());

    LibraryScanner scanner{fixture.dbPool, fixture.playlistLoader, fixture.metadataStore, fixture.audioLoader, nullptr};
    scanner.initialiseThread();

    ScanResult result;
    QObject::connect(&scanner, &LibraryScanner::scanUpdate, &scanner,
                     [&result](const ScanResult& update) { result = update; });

    scanner.scanTracks(seeded, false, {});

    EXPECT_TRUE(result.updatedTracks.empty());
    EXPECT_TRUE(result.addedTracks.empty());
}

TEST(LibraryScannerTest, MatchingCuePrefersCueNamedForTargetFile)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString flacPath       = dir.filePath(u"Abbey Road.flac"_s);
    const QString genericCuePath = dir.filePath(u"Abbey Road.cue"_s);
    const QString exactCuePath   = dir.filePath(u"Abbey Road.flac.cue"_s);

    writeFile(flacPath, "flac");
    writeFile(genericCuePath, "FILE \"Abbey Road.wav\" WAVE\n");
    writeFile(exactCuePath, "FILE \"Abbey Road.flac\" WAVE\n");

    const auto match = findMatchingCue(QFileInfo{flacPath});
    ASSERT_TRUE(match.has_value());
    EXPECT_EQ(normalisePath(exactCuePath), normalisePath(match->absoluteFilePath()));
}

TEST(LibraryScannerTest, LocalCueDoesNotBlockEmbeddedCueTracks)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString flacPath = dir.filePath(u"album.flac"_s);
    const QString cuePath  = dir.filePath(u"album.cue"_s);

    writeFile(flacPath, "flac");
    writeFile(cuePath, "FILE \"album.flac\" WAVE\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Local One\"\n"
                       "    INDEX 01 00:00:00\n");

    PlaylistLoader playlistLoader;
    playlistLoader.addParser(std::make_unique<CueParser>());

    AudioLoader audioLoader;
    const auto readerState = std::make_shared<FakeEmbeddedCueReader::State>();
    readerState->cueSheet  = uR"(FILE "album.flac" FLAC
  TRACK 01 AUDIO
    TITLE "Embedded One"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Embedded Two"
    INDEX 01 01:00:00
)"_s;
    audioLoader.addReader(
        u"fake-embedded"_s, [readerState]() { return std::make_unique<FakeEmbeddedCueReader>(readerState); }, 0);

    DummyScanHost host;
    LibraryScanState state{&host};
    LibraryScanWriter writer{nullptr, [](const ScanResult&) { }};
    auto metadataStore = std::make_shared<Fooyin::TrackMetadataStore>();
    LibraryTrackResolver resolver{{},      &playlistLoader, &audioLoader, false, metadataStore,
                                  nullptr, &state,          &writer,      {},    [] { }};

    resolver.readCue(QFileInfo{cuePath}, false);
    const int callsAfterCue = readerState->readCalls;

    writer.reset();
    resolver.readFile(QFileInfo{flacPath}, false);

    EXPECT_GT(readerState->readCalls, callsAfterCue);
    EXPECT_FALSE(writer.empty());
}

TEST(LibraryScannerTest, DroppingLocalCueAndEmbeddedFileAddsOnlyEmbeddedCueTracks)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString flacPath = dir.filePath(u"album.flac"_s);
    const QString cuePath  = dir.filePath(u"album.cue"_s);

    writeFile(flacPath, "flac");
    writeFile(cuePath, "FILE \"album.flac\" WAVE\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Local One\"\n"
                       "    PERFORMER \"Local Artist\"\n"
                       "    INDEX 01 00:00:00\n");

    auto playlistLoader = std::make_shared<PlaylistLoader>();
    playlistLoader->addParser(std::make_unique<CueParser>());

    auto audioLoader       = std::make_shared<AudioLoader>();
    const auto readerState = std::make_shared<FakeEmbeddedCueReader::State>();
    readerState->cueSheet  = uR"(PERFORMER "Embedded Artist"
FILE "album.flac" FLAC
  TRACK 01 AUDIO
    TITLE "Embedded One"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Embedded Two"
    INDEX 01 01:00:00
)"_s;
    audioLoader->addReader(
        u"fake-embedded"_s, [readerState]() { return std::make_unique<FakeEmbeddedCueReader>(readerState); }, 0);

    LibraryScanConfig config;
    config.externalRestrictExt = {u"cue"_s, u"flac"_s};
    DummyScanHost host;
    auto metadataStore = std::make_shared<Fooyin::TrackMetadataStore>();
    LibraryScanSession session{nullptr, playlistLoader.get(), audioLoader.get(), metadataStore, config, &host};
    LibraryScanFilesResult result;

    ASSERT_TRUE(session.scanFiles({}, {QUrl::fromLocalFile(cuePath), QUrl::fromLocalFile(flacPath)}, result));
    sortTracks(result.tracksScanned);
    const TrackList& scannedTracks = result.tracksScanned;

    ASSERT_EQ(2, scannedTracks.size());
    EXPECT_EQ(u"Embedded Artist"_s, scannedTracks.at(0).artist());
    EXPECT_EQ(u"Embedded Artist"_s, scannedTracks.at(1).artist());
    EXPECT_TRUE(scannedTracks.at(0).albumArtist().isEmpty());
    EXPECT_TRUE(scannedTracks.at(1).albumArtist().isEmpty());
    EXPECT_EQ(u"Embedded One"_s, scannedTracks.at(0).title());
    EXPECT_EQ(u"Embedded Two"_s, scannedTracks.at(1).title());
}

TEST(LibraryScannerTest, DroppingLocalCueAndBackingFileAddsOnlyCueTracks)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString binPath = dir.filePath(u"album.bin"_s);
    const QString cuePath = dir.filePath(u"album.cue"_s);

    writeFile(binPath, "bin");
    writeFile(cuePath, "PERFORMER \"Local Artist\"\n"
                       "TITLE \"Local Album\"\n"
                       "FILE \"album.bin\" BINARY\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Local One\"\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    TITLE \"Local Two\"\n"
                       "    INDEX 01 01:00:00\n");

    auto playlistLoader = std::make_shared<PlaylistLoader>();
    playlistLoader->addParser(std::make_unique<CueParser>());

    auto audioLoader       = std::make_shared<AudioLoader>();
    const auto readerState = std::make_shared<FakeEmbeddedCueReader::State>();
    audioLoader->addReader(
        u"fake-embedded"_s, [readerState]() { return std::make_unique<FakeEmbeddedCueReader>(readerState); }, 0);

    LibraryScanConfig config;
    config.externalRestrictExt = {u"cue"_s, u"bin"_s};
    DummyScanHost host;
    auto metadataStore = std::make_shared<Fooyin::TrackMetadataStore>();
    LibraryScanSession session{nullptr, playlistLoader.get(), audioLoader.get(), metadataStore, config, &host};
    LibraryScanFilesResult result;

    ASSERT_TRUE(session.scanFiles({}, {QUrl::fromLocalFile(cuePath), QUrl::fromLocalFile(binPath)}, result));
    sortTracks(result.tracksScanned);
    const TrackList& scannedTracks = result.tracksScanned;

    ASSERT_EQ(2, scannedTracks.size());
    EXPECT_EQ(u"Local One"_s, scannedTracks.at(0).title());
    EXPECT_EQ(u"Local Two"_s, scannedTracks.at(1).title());
    EXPECT_EQ(u"Local Artist"_s, scannedTracks.at(0).artist());
    EXPECT_EQ(u"Local Artist"_s, scannedTracks.at(1).artist());
    EXPECT_TRUE(scannedTracks.at(0).albumArtist().isEmpty());
    EXPECT_TRUE(scannedTracks.at(1).albumArtist().isEmpty());
}

TEST(LibraryScannerTest, DroppingBackingFileBeforeLocalCueAddsOnlyCueTracks)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString binPath = dir.filePath(u"album.bin"_s);
    const QString cuePath = dir.filePath(u"album.cue"_s);

    writeFile(binPath, "bin");
    writeFile(cuePath, "PERFORMER \"Local Artist\"\n"
                       "TITLE \"Local Album\"\n"
                       "FILE \"album.bin\" BINARY\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Local One\"\n"
                       "    INDEX 01 00:00:00\n"
                       "  TRACK 02 AUDIO\n"
                       "    TITLE \"Local Two\"\n"
                       "    INDEX 01 01:00:00\n");

    auto playlistLoader = std::make_shared<PlaylistLoader>();
    playlistLoader->addParser(std::make_unique<CueParser>());

    auto audioLoader       = std::make_shared<AudioLoader>();
    const auto readerState = std::make_shared<FakeEmbeddedCueReader::State>();
    audioLoader->addReader(
        u"fake-embedded"_s, [readerState]() { return std::make_unique<FakeEmbeddedCueReader>(readerState); }, 0);

    LibraryScanConfig config;
    config.externalRestrictExt = {u"cue"_s, u"bin"_s};
    DummyScanHost host;
    auto metadataStore = std::make_shared<Fooyin::TrackMetadataStore>();
    LibraryScanSession session{nullptr, playlistLoader.get(), audioLoader.get(), metadataStore, config, &host};
    LibraryScanFilesResult result;

    ASSERT_TRUE(session.scanFiles({}, {QUrl::fromLocalFile(binPath), QUrl::fromLocalFile(cuePath)}, result));
    sortTracks(result.tracksScanned);
    const TrackList& scannedTracks = result.tracksScanned;

    ASSERT_EQ(2, scannedTracks.size());
    EXPECT_EQ(u"Local One"_s, scannedTracks.at(0).title());
    EXPECT_EQ(u"Local Two"_s, scannedTracks.at(1).title());
    EXPECT_EQ(u"Local Artist"_s, scannedTracks.at(0).artist());
    EXPECT_EQ(u"Local Artist"_s, scannedTracks.at(1).artist());
    EXPECT_TRUE(scannedTracks.at(0).albumArtist().isEmpty());
    EXPECT_TRUE(scannedTracks.at(1).albumArtist().isEmpty());
}

TEST(LibraryScannerTest, DroppingDirectoryWithLocalCueAndEmbeddedFileAddsOnlyEmbeddedCueTracks)
{
    ensureCoreApplication();

    const QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString flacPath = dir.filePath(u"album.flac"_s);
    const QString cuePath  = dir.filePath(u"album.cue"_s);

    writeFile(flacPath, "flac");
    writeFile(cuePath, "FILE \"album.flac\" WAVE\n"
                       "  TRACK 01 AUDIO\n"
                       "    TITLE \"Local One\"\n"
                       "    PERFORMER \"Local Artist\"\n"
                       "    INDEX 01 00:00:00\n");

    auto playlistLoader = std::make_shared<PlaylistLoader>();
    playlistLoader->addParser(std::make_unique<CueParser>());

    auto audioLoader       = std::make_shared<AudioLoader>();
    const auto readerState = std::make_shared<FakeEmbeddedCueReader::State>();
    readerState->cueSheet  = uR"(PERFORMER "Embedded Artist"
FILE "album.flac" FLAC
  TRACK 01 AUDIO
    TITLE "Embedded One"
    INDEX 01 00:00:00
  TRACK 02 AUDIO
    TITLE "Embedded Two"
    INDEX 01 01:00:00
)"_s;
    audioLoader->addReader(
        u"fake-embedded"_s, [readerState]() { return std::make_unique<FakeEmbeddedCueReader>(readerState); }, 0);

    LibraryScanConfig config;
    config.externalRestrictExt = {u"cue"_s, u"flac"_s};
    DummyScanHost host;
    auto metadataStore = std::make_shared<Fooyin::TrackMetadataStore>();
    LibraryScanSession session{nullptr, playlistLoader.get(), audioLoader.get(), metadataStore, config, &host};
    LibraryScanFilesResult result;

    ASSERT_TRUE(session.scanFiles({}, {QUrl::fromLocalFile(dir.path())}, result));
    sortTracks(result.tracksScanned);
    const TrackList& scannedTracks = result.tracksScanned;

    ASSERT_EQ(2, scannedTracks.size());
    EXPECT_EQ(u"Embedded Artist"_s, scannedTracks.at(0).artist());
    EXPECT_EQ(u"Embedded Artist"_s, scannedTracks.at(1).artist());
    EXPECT_TRUE(scannedTracks.at(0).albumArtist().isEmpty());
    EXPECT_TRUE(scannedTracks.at(1).albumArtist().isEmpty());
    EXPECT_EQ(u"Embedded One"_s, scannedTracks.at(0).title());
    EXPECT_EQ(u"Embedded Two"_s, scannedTracks.at(1).title());
}
} // namespace Fooyin::Testing
