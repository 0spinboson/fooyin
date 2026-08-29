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

#include "libraryscanutils.h"

#include "playlist/parsers/cueparser.h"

#include <QBuffer>
#include <QDir>
#include <QLoggingCategory>
#include <QRegularExpression>

#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(LIB_SCANNER)

using namespace Qt::StringLiterals;

namespace {
bool pathIsWithinRoot(const QString& path, const QString& root)
{
    return path == root || path.startsWith(root + u"/"_s);
}

int cueMatchRank(const QFileInfo& file, const QFileInfo& cue)
{
    const QString fileName = file.fileName();
    const QString fileBase = file.completeBaseName();
    const QString cueName  = cue.fileName();
    const QString cueBase  = cue.completeBaseName();

    if(cueName.compare(fileName + u".cue"_s, Qt::CaseInsensitive) == 0
       || cueBase.compare(fileName, Qt::CaseInsensitive) == 0) {
        return 0;
    }
    if(cueBase.compare(fileBase, Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if(cueName.contains(fileName, Qt::CaseInsensitive)) {
        return 2;
    }
    if(cueBase.contains(fileBase, Qt::CaseInsensitive)) {
        return 3;
    }

    return -1;
}

bool pathIsWithinRoots(const QString& path, const QStringList& roots)
{
    if(roots.empty()) {
        return true;
    }

    return std::ranges::any_of(roots, [&path](const QString& root) { return pathIsWithinRoot(path, root); });
}

uint64_t minNonZero(const uint64_t lhs, const uint64_t rhs)
{
    if(lhs == 0) {
        return rhs;
    }
    if(rhs == 0) {
        return lhs;
    }
    return std::min(lhs, rhs);
}
} // namespace

namespace Fooyin {
QString normalisePath(const QString& path)
{
    if(path.isEmpty()) {
        return {};
    }

    if(Track::isArchivePath(path)) {
        return path;
    }

    const QFileInfo info{path};
    return QDir::cleanPath(info.absoluteFilePath());
}

QStringList normalisePaths(const QStringList& paths)
{
    QStringList normalised;
    normalised.reserve(paths.size());

    for(const auto& path : paths) {
        const QString normalisedPath = normalisePath(path);
        if(!normalisedPath.isEmpty()) {
            normalised.push_back(normalisedPath);
        }
    }

    return normalised;
}

QStringList normaliseExtensions(const QStringList& extensions)
{
    QStringList normalised;
    normalised.reserve(extensions.size());

    for(const auto& extension : extensions) {
        const QString value = extension.trimmed().toLower();
        if(!value.isEmpty()) {
            normalised.push_back(value);
        }
    }

    return normalised;
}

QString trackIdentity(const Track& track)
{
    if(track.id() >= 0) {
        return QString::number(track.id());
    }

    return track.uniqueFilepath() + u'|' + track.hash() + u'|' + QString::number(track.duration());
}

QString physicalTrackPath(const Track& track)
{
    return track.isInArchive() ? normalisePath(track.archivePath()) : normalisePath(track.filepath());
}

bool trackIsInRoots(const Track& track, const QStringList& roots)
{
    if(roots.empty()) {
        return true;
    }

    if(pathIsWithinRoots(physicalTrackPath(track), roots)) {
        return true;
    }

    if(track.hasCue() && !track.hasEmbeddedCue()) {
        return pathIsWithinRoots(normalisePath(track.cuePath()), roots);
    }

    return false;
}

std::optional<QFileInfo> findMatchingCue(const QFileInfo& file)
{
    static const QStringList cueExtensions{u"*.cue"_s};

    const QDir dir           = file.absoluteDir();
    const QFileInfoList cues = dir.entryInfoList(cueExtensions, QDir::Files, QDir::Name | QDir::IgnoreCase);

    return findMatchingCue(file, cues);
}

std::optional<QFileInfo> findMatchingCue(const QFileInfo& file, const QFileInfoList& cueFiles)
{
    std::optional<QFileInfo> bestCue;
    int bestRank{std::numeric_limits<int>::max()};

    for(const auto& cue : cueFiles) {
        const int rank = cueMatchRank(file, cue);
        if(rank < 0 || rank >= bestRank) {
            continue;
        }

        bestCue  = cue;
        bestRank = rank;
    }

    return bestCue;
}

void readFileProperties(Track& track)
{
    const QFileInfo fileInfo{physicalTrackPath(track)};

    if(track.addedTime() == 0) {
        track.setAddedTime(QDateTime::currentMSecsSinceEpoch());
    }
    if(track.createdTime() == 0) {
        const QDateTime createdTime = fileInfo.birthTime();
        track.setCreatedTime(createdTime.isValid() ? createdTime.toMSecsSinceEpoch() : 0);
    }
    if(track.modifiedTime() == 0) {
        const QDateTime modifiedTime = fileInfo.lastModified();
        track.setModifiedTime(modifiedTime.isValid() ? modifiedTime.toMSecsSinceEpoch() : 0);
    }
    if(track.fileSize() == 0) {
        track.setFileSize(fileInfo.size());
    }
}

void mergeReloadedTrackStats(Track& track, const Track& existingTrack, const TrackReloadOptions& options)
{
    const bool fileHasRating    = track.rating() > 0;
    const bool fileHasPlayStats = track.playCount() > 0 || track.firstPlayed() > 0 || track.lastPlayed() > 0;

    if(options.overwriteRatingOnReload) {
        if(!fileHasRating && existingTrack.rating() > 0) {
            track.setRating(existingTrack.rating());
        }
    }
    else if(existingTrack.rating() > 0 || !fileHasRating) {
        track.setRating(existingTrack.rating());
    }

    if(options.overwritePlaycountOnReload) {
        if(!fileHasPlayStats) {
            track.setPlayCount(existingTrack.playCount());
            track.setFirstPlayed(existingTrack.firstPlayed());
            track.setLastPlayed(existingTrack.lastPlayed());
            return;
        }

        if(track.playCount() <= 0) {
            track.setPlayCount(existingTrack.playCount());
        }
        if(track.firstPlayed() == 0) {
            track.setFirstPlayed(existingTrack.firstPlayed());
        }
        if(track.lastPlayed() == 0) {
            track.setLastPlayed(existingTrack.lastPlayed());
        }
        return;
    }

    track.setPlayCount(std::max(existingTrack.playCount(), track.playCount()));
    track.setFirstPlayed(minNonZero(existingTrack.firstPlayed(), track.firstPlayed()));
    track.setLastPlayed(std::max(existingTrack.lastPlayed(), track.lastPlayed()));
}

TrackList parseEmbeddedCueSheet(const Track& parentTrack, const PlaylistParser::ReadPlaylistEntry& readEntry)
{
    const QStringList cues = parentTrack.extraTag(u"CUESHEET"_s);
    if(cues.empty()) {
        return {};
    }

    QByteArray bytes{cues.front().toUtf8()};
    QBuffer buffer{&bytes};
    if(!buffer.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCInfo(LIB_SCANNER) << "Can't open cue sheet buffer for reading:" << buffer.errorString();
        return {};
    }

    // A default-constructed QDir routes readPlaylist() to CueParser's embedded-cue path.
    CueParser parser;
    TrackList tracks = parser.readPlaylist(&buffer, parentTrack.filepath(), {}, readEntry, false);
    applyCueTrackTags(parentTrack, tracks);

    return tracks;
}

std::vector<std::optional<qsizetype>> matchReloadedCueTracks(const TrackList& reloadedTracks,
                                                             const TrackList& existingTracks)
{
    std::vector<std::optional<qsizetype>> matches(reloadedTracks.size());
    std::vector<bool> claimed(existingTracks.size(), false);

    // Physical identity is the stronger signal, so resolve it across every track before falling
    // back. Claiming as we go keeps two reloaded tracks from targeting the same database row.
    for(qsizetype i{0}; i < std::ssize(reloadedTracks); ++i) {
        for(qsizetype j{0}; j < std::ssize(existingTracks); ++j) {
            if(!claimed[j] && existingTracks[j].sameIdentityAs(reloadedTracks[i])) {
                matches[i] = j;
                claimed[j] = true;
                break;
            }
        }
    }

    // Track number covers cue sheets whose offsets moved, over the unclaimed remainder only.
    for(qsizetype i{0}; i < std::ssize(reloadedTracks); ++i) {
        if(matches[i].has_value()) {
            continue;
        }

        const QString trackNumber = reloadedTracks[i].trackNumber();
        if(trackNumber.isEmpty()) {
            continue;
        }

        for(qsizetype j{0}; j < std::ssize(existingTracks); ++j) {
            if(!claimed[j] && existingTracks[j].trackNumber() == trackNumber) {
                matches[i] = j;
                claimed[j] = true;
                break;
            }
        }
    }

    return matches;
}

void applyCueTrackTags(const Track& parentTrack, TrackList& cueTracks)
{
    if(cueTracks.empty()) {
        return;
    }

    static const QRegularExpression cueTagRegex{u"^CUE_TRACK(\\d+)_(.+)$"_s, QRegularExpression::CaseInsensitiveOption};

    std::unordered_map<int, std::vector<std::pair<QString, QStringList>>> tagsByTrackNum;
    bool foundCueTags{false};

    const Track::ExtraTags parentTags = parentTrack.extraTags();
    for(const auto& [key, values] : parentTags.entries()) {
        const QRegularExpressionMatch match = cueTagRegex.match(key);
        if(!match.hasMatch()) {
            continue;
        }

        foundCueTags = true;

        bool numberOk{false};
        const int trackNum = match.captured(1).toInt(&numberOk);
        if(numberOk && !values.empty()) {
            tagsByTrackNum[trackNum].emplace_back(match.captured(2).toUpper(), values);
        }
    }

    if(!foundCueTags) {
        return;
    }

    for(Track& cueTrack : cueTracks) {
        // Every generated track inherits the parent file's tags, so each one carries the Cue_track* tags for
        // all other tracks. Rebuild the extra tags without them rather than using removeExtraTag(), which
        // would record them in removedTags() and cause a later tag write to delete them from the file.
        const Track::ExtraTags inheritedTags = cueTrack.extraTags();
        cueTrack.clearExtraTags();
        for(const auto& [key, values] : inheritedTags.entries()) {
            if(!cueTagRegex.match(key).hasMatch()) {
                cueTrack.addExtraTag(key, values);
            }
        }

        bool numberOk{false};
        const int trackNum = cueTrack.trackNumber().toInt(&numberOk);
        if(!numberOk) {
            continue;
        }

        const auto trackTags = tagsByTrackNum.find(trackNum);
        if(trackTags == tagsByTrackNum.cend()) {
            continue;
        }

        for(const auto& [field, values] : trackTags->second) {
            if(field == "ARTIST"_L1) {
                cueTrack.setArtists(values);
            }
            else if(field == "ALBUMARTIST"_L1) {
                cueTrack.setAlbumArtists(values);
            }
            else if(field == "COMPOSER"_L1) {
                cueTrack.setComposers(values);
            }
            else if(field == "PERFORMER"_L1) {
                cueTrack.setPerformers(values);
            }
            else if(field == "GENRE"_L1) {
                cueTrack.setGenres(values);
            }
            else if(field == "TITLE"_L1) {
                cueTrack.setTitle(values.front());
            }
            else if(field == "DATE"_L1) {
                cueTrack.setDate(values.front());
            }
            else if(field == "COMMENT"_L1) {
                cueTrack.setComment(values.front());
            }
            else if(Track::isExtraTag(field)) {
                cueTrack.replaceExtraTag(field, values);
            }
            else {
                // Never let a reserved name into extraTags: the tag writer replays extra tags over
                // the canonical fields it just wrote, so an extra tag named TITLE or RATING would
                // overwrite the real value in the user's file.
                qCDebug(LIB_SCANNER) << "Ignoring reserved field in Cue_track tag:" << field;
            }
        }
    }
}
} // namespace Fooyin
