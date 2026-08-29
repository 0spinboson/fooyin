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

#pragma once

#include "fycore_export.h"

#include <core/playlist/playlistparser.h>
#include <core/track.h>

#include <QFileInfo>

#include <optional>
#include <vector>

namespace Fooyin {
struct FYCORE_EXPORT TrackReloadOptions
{
    bool overwriteRatingOnReload{false};
    bool overwritePlaycountOnReload{false};
};

FYCORE_EXPORT QString normalisePath(const QString& path);
FYCORE_EXPORT QStringList normalisePaths(const QStringList& paths);
FYCORE_EXPORT QStringList normaliseExtensions(const QStringList& extensions);
FYCORE_EXPORT QString trackIdentity(const Track& track);
FYCORE_EXPORT QString physicalTrackPath(const Track& track);
FYCORE_EXPORT bool trackIsInRoots(const Track& track, const QStringList& roots);
FYCORE_EXPORT std::optional<QFileInfo> findMatchingCue(const QFileInfo& file, const QFileInfoList& cueFiles);
FYCORE_EXPORT std::optional<QFileInfo> findMatchingCue(const QFileInfo& file);
FYCORE_EXPORT void readFileProperties(Track& track);
FYCORE_EXPORT void mergeReloadedTrackStats(Track& track, const Track& existingTrack, const TrackReloadOptions& options);
/*!
 * Applies per-track Cue_trackNN_* tags from @p parentTrack to the tracks generated from its embedded cue sheet.
 *
 * APEv2 files with an embedded CUESHEET can store per-track metadata as separate Cue_track01_composer,
 * Cue_track02_performer, etc. tags. These are matched to @p cueTracks by track number and applied to the
 * corresponding field, then stripped from every generated track so they don't appear as inherited clutter.
 */
FYCORE_EXPORT void applyCueTrackTags(const Track& parentTrack, TrackList& cueTracks);
/*!
 * Maps each reloaded cue track to the index of the existing track it should update.
 *
 * Identity matches (physical segment) are resolved across all tracks first; a track-number
 * fallback then runs over the tracks left unclaimed, covering cue sheets whose offsets changed.
 * Each existing track is claimed at most once, so two reloaded tracks can never target the same
 * database row. Returns std::nullopt for a reloaded track with no match.
 */
FYCORE_EXPORT std::vector<std::optional<qsizetype>> matchReloadedCueTracks(const TrackList& reloadedTracks,
                                                                           const TrackList& existingTracks);
/*!
 * Parses @p parentTrack's embedded CUESHEET tag into its constituent tracks.
 *
 * Applies applyCueTrackTags() to the result, so every caller that expands an embedded cue sheet
 * gets the same per-track metadata. Returns an empty list when there is no cue sheet to parse.
 */
FYCORE_EXPORT TrackList parseEmbeddedCueSheet(const Track& parentTrack,
                                              const PlaylistParser::ReadPlaylistEntry& readEntry);
} // namespace Fooyin
