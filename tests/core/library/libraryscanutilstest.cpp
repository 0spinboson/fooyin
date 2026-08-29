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

#include "core/library/libraryscanutils.h"

#include <core/track.h>

#include <gtest/gtest.h>

using namespace Qt::StringLiterals;

namespace Fooyin::Testing {
namespace {
Track makeCueTrack(const QString& trackNumber, const uint64_t offset = 0, const uint64_t duration = 1000)
{
    Track track{u"/music/album.tak"_s};
    track.setTrackNumber(trackNumber);
    track.setOffset(offset);
    track.setDuration(duration);
    track.setCuePath(u"Embedded"_s);
    return track;
}

Track makeParent(const std::vector<std::pair<QString, QString>>& tags)
{
    Track parent{u"/music/album.tak"_s};
    for(const auto& [key, value] : tags) {
        parent.addExtraTag(key, value);
    }
    return parent;
}
} // namespace

// ---------------------------------------------------------------------------
// applyCueTrackTags
// ---------------------------------------------------------------------------

TEST(LibraryScanUtilsTest, AppliesCueTrackTagToMatchingTrackOnly)
{
    const Track parent
        = makeParent({{u"Cue_track01_composer"_s, u"Monteverdi"_s}, {u"Cue_track02_composer"_s, u"Gabrieli"_s}});

    TrackList tracks{makeCueTrack(u"01"_s), makeCueTrack(u"02"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(QStringList{u"Monteverdi"_s}, tracks.at(0).composers());
    EXPECT_EQ(QStringList{u"Gabrieli"_s}, tracks.at(1).composers());
}

TEST(LibraryScanUtilsTest, MatchesCueTrackTagKeysCaseInsensitively)
{
    const Track parent = makeParent({{u"CUE_TRACK01_PERFORMER"_s, u"Upper"_s},
                                     {u"cue_track02_performer"_s, u"Lower"_s},
                                     {u"Cue_Track03_Performer"_s, u"Mixed"_s}});

    TrackList tracks{makeCueTrack(u"01"_s), makeCueTrack(u"02"_s), makeCueTrack(u"03"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(QStringList{u"Upper"_s}, tracks.at(0).performers());
    EXPECT_EQ(QStringList{u"Lower"_s}, tracks.at(1).performers());
    EXPECT_EQ(QStringList{u"Mixed"_s}, tracks.at(2).performers());
}

TEST(LibraryScanUtilsTest, MatchesZeroPaddedAndUnpaddedTrackNumbers)
{
    const Track parent = makeParent({{u"Cue_track1_composer"_s, u"Unpadded"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(QStringList{u"Unpadded"_s}, tracks.at(0).composers());
}

TEST(LibraryScanUtilsTest, StripsCueTrackTagsFromEveryGeneratedTrack)
{
    const Track parent = makeParent({{u"Cue_track01_composer"_s, u"A"_s}, {u"Cue_track02_composer"_s, u"B"_s}});

    // Each generated track inherits the parent's full tag set.
    TrackList tracks{makeCueTrack(u"01"_s), makeCueTrack(u"02"_s)};
    for(Track& track : tracks) {
        track.addExtraTag(u"Cue_track01_composer"_s, u"A"_s);
        track.addExtraTag(u"Cue_track02_composer"_s, u"B"_s);
    }

    applyCueTrackTags(parent, tracks);

    for(const Track& track : tracks) {
        EXPECT_FALSE(track.hasExtraTag(u"CUE_TRACK01_COMPOSER"_s));
        EXPECT_FALSE(track.hasExtraTag(u"CUE_TRACK02_COMPOSER"_s));
    }
}

// Regression test for the reason clearExtraTags()+rebuild is used instead of removeExtraTag():
// removeExtraTag() records the key in removedTags(), which tag writers consume to DELETE the tag
// from the user's file. If this ever regresses, reloading a cue album silently strips its
// Cue_trackNN_* tags from disk on the next tag write.
TEST(LibraryScanUtilsTest, DoesNotRecordRemovedTagsWhenStrippingCueTrackTags)
{
    const Track parent = makeParent({{u"Cue_track01_composer"_s, u"A"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    tracks[0].addExtraTag(u"Cue_track01_composer"_s, u"A"_s);
    tracks[0].addExtraTag(u"Cue_track02_composer"_s, u"B"_s);
    ASSERT_TRUE(tracks.at(0).removedTags().isEmpty());

    applyCueTrackTags(parent, tracks);

    EXPECT_TRUE(tracks.at(0).removedTags().isEmpty());
}

TEST(LibraryScanUtilsTest, PreservesUnrelatedMultiValueExtraTags)
{
    const Track parent = makeParent({{u"Cue_track01_composer"_s, u"A"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    tracks[0].addExtraTag(u"Cue_track01_composer"_s, u"A"_s);
    tracks[0].addExtraTag(u"MOOD"_s, QStringList{u"calm"_s, u"sacred"_s});

    applyCueTrackTags(parent, tracks);

    EXPECT_EQ((QStringList{u"calm"_s, u"sacred"_s}), tracks.at(0).extraTag(u"MOOD"_s));
}

TEST(LibraryScanUtilsTest, IgnoresCueTrackTagsWithNoMatchingTrack)
{
    const Track parent = makeParent({{u"Cue_track05_composer"_s, u"Orphan"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_TRUE(tracks.at(0).composers().isEmpty());
}

TEST(LibraryScanUtilsTest, IgnoresTracksWithEmptyOrNonNumericTrackNumber)
{
    const Track parent = makeParent({{u"Cue_track01_composer"_s, u"A"_s}});

    TrackList tracks{makeCueTrack(QString{}), makeCueTrack(u"A1"_s)};
    for(Track& track : tracks) {
        track.addExtraTag(u"Cue_track01_composer"_s, u"A"_s);
    }

    applyCueTrackTags(parent, tracks);

    for(const Track& track : tracks) {
        EXPECT_TRUE(track.composers().isEmpty());
        // Stripping must still happen even when nothing is applied.
        EXPECT_FALSE(track.hasExtraTag(u"CUE_TRACK01_COMPOSER"_s));
    }
}

TEST(LibraryScanUtilsTest, LeavesTracksUntouchedWhenParentHasNoCueTrackTags)
{
    const Track parent = makeParent({{u"ALBUM"_s, u"Vespers"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    tracks[0].addExtraTag(u"MOOD"_s, u"calm"_s);

    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(QStringList{u"calm"_s}, tracks.at(0).extraTag(u"MOOD"_s));
    EXPECT_TRUE(tracks.at(0).composers().isEmpty());
}

// Real-world case: the fields an APEv2 classical rip actually carries.
TEST(LibraryScanUtilsTest, MapsNonCanonicalFieldsToExtraTags)
{
    const Track parent
        = makeParent({{u"Cue_track01_conductor"_s, u"Gardiner"_s}, {u"Cue_track01_ensemble"_s, u"Monteverdi Choir"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(QStringList{u"Gardiner"_s}, tracks.at(0).extraTag(u"CONDUCTOR"_s));
    EXPECT_EQ(QStringList{u"Monteverdi Choir"_s}, tracks.at(0).extraTag(u"ENSEMBLE"_s));
}

// A Cue_trackNN_title tag must never become an extra tag literally named TITLE: the tag writer
// applies canonical fields first and then replays extraTags over them, so an extra tag named
// TITLE overwrites the real title in the user's file.
TEST(LibraryScanUtilsTest, NeverInjectsReservedFieldNamesIntoExtraTags)
{
    const Track parent = makeParent({{u"Cue_track01_title"_s, u"Deposuit potentes"_s},
                                     {u"Cue_track01_date"_s, u"1610"_s},
                                     {u"Cue_track01_rating"_s, u"5"_s},
                                     {u"Cue_track01_albumartist"_s, u"Gardiner"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_FALSE(tracks.at(0).hasExtraTag(u"TITLE"_s));
    EXPECT_FALSE(tracks.at(0).hasExtraTag(u"DATE"_s));
    EXPECT_FALSE(tracks.at(0).hasExtraTag(u"RATING"_s));
    EXPECT_FALSE(tracks.at(0).hasExtraTag(u"ALBUMARTIST"_s));
}

TEST(LibraryScanUtilsTest, AppliesCanonicalFieldsToTheirDedicatedSetters)
{
    const Track parent = makeParent({{u"Cue_track01_title"_s, u"Deposuit potentes"_s},
                                     {u"Cue_track01_date"_s, u"1610"_s},
                                     {u"Cue_track01_albumartist"_s, u"Gardiner"_s},
                                     {u"Cue_track01_comment"_s, u"Live"_s}});

    TrackList tracks{makeCueTrack(u"01"_s)};
    applyCueTrackTags(parent, tracks);

    EXPECT_EQ(u"Deposuit potentes"_s, tracks.at(0).title());
    EXPECT_EQ(u"1610"_s, tracks.at(0).date());
    EXPECT_EQ(QStringList{u"Gardiner"_s}, tracks.at(0).albumArtists());
    EXPECT_EQ(u"Live"_s, tracks.at(0).comment());
}

// ---------------------------------------------------------------------------
// matchReloadedCueTracks
// ---------------------------------------------------------------------------

TEST(LibraryScanUtilsTest, MatchesReloadedTracksByPhysicalIdentity)
{
    TrackList existing{makeCueTrack(u"01"_s, 0, 1000), makeCueTrack(u"02"_s, 1000, 2000)};
    TrackList reloaded{makeCueTrack(u"01"_s, 0, 1000), makeCueTrack(u"02"_s, 1000, 2000)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(2U, matches.size());
    EXPECT_EQ(0, matches.at(0));
    EXPECT_EQ(1, matches.at(1));
}

TEST(LibraryScanUtilsTest, FallsBackToTrackNumberWhenOffsetsChanged)
{
    TrackList existing{makeCueTrack(u"01"_s, 0, 1000)};
    TrackList reloaded{makeCueTrack(u"01"_s, 40, 960)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(1U, matches.size());
    EXPECT_EQ(0, matches.at(0));
}

TEST(LibraryScanUtilsTest, ReturnsNoMatchForUnmatchedReloadedTracks)
{
    TrackList existing{makeCueTrack(u"01"_s, 0, 1000)};
    TrackList reloaded{makeCueTrack(u"01"_s, 0, 1000), makeCueTrack(u"02"_s, 1000, 2000)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(2U, matches.size());
    EXPECT_EQ(0, matches.at(0));
    EXPECT_FALSE(matches.at(1).has_value());
}

// Two reloaded tracks must never claim the same existing row. Without consumption the
// track-number fallback re-matches a row already taken by an identity match, so two updates
// target one database id: one silently overwrites the other and a stale row is left behind.
TEST(LibraryScanUtilsTest, NeverClaimsTheSameExistingTrackTwice)
{
    // Existing row is old TRACK 02 sitting at offset 0.
    TrackList existing{makeCueTrack(u"02"_s, 0, 1000)};

    // Re-cut sheet: TRACK 01 now occupies offset 0 (identity match), TRACK 02 moved elsewhere.
    TrackList reloaded{makeCueTrack(u"01"_s, 0, 1000), makeCueTrack(u"02"_s, 5000, 1000)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(2U, matches.size());
    EXPECT_EQ(0, matches.at(0));
    EXPECT_FALSE(matches.at(1).has_value()) << "existing track 0 was already claimed by an identity match";
}

// Identity is the stronger signal and must win globally, not merely per-track: a track-number
// fallback resolved early must not steal a row that a later track matches exactly.
TEST(LibraryScanUtilsTest, ResolvesAllIdentityMatchesBeforeTrackNumberFallback)
{
    TrackList existing{makeCueTrack(u"01"_s, 9000, 1000)};

    // First reloaded track shares the number but not the position; second matches exactly.
    TrackList reloaded{makeCueTrack(u"01"_s, 0, 1000), makeCueTrack(u"01"_s, 9000, 1000)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(2U, matches.size());
    EXPECT_FALSE(matches.at(0).has_value());
    EXPECT_EQ(0, matches.at(1)) << "the exact identity match must win the row";
}

TEST(LibraryScanUtilsTest, DoesNotMatchOnEmptyTrackNumbers)
{
    TrackList existing{makeCueTrack(QString{}, 0, 1000)};
    TrackList reloaded{makeCueTrack(QString{}, 5000, 2000)};

    const auto matches = matchReloadedCueTracks(reloaded, existing);

    ASSERT_EQ(1U, matches.size());
    EXPECT_FALSE(matches.at(0).has_value());
}
} // namespace Fooyin::Testing
