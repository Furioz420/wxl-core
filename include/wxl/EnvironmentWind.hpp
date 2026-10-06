// The wind: one field, shared by everything that answers to it.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <cstdint>

// ONE WIND, READ BY EVERYTHING.
//
// Before this there were two, and they did not know about each other: the grass had a heading and a
// speed, the sea had a heading and a speed, and nothing made them agree. Two settings that mean the
// same thing and can hold different values is not a tuning surface, it is a bug waiting for somebody
// to notice the grass leaning one way while the swell runs the other.
//
// WHY IT IS A CLOSED FORM OF (SEED, TIME), and why that is not negotiable.
//
// The sea is already a closed form: hand two machines the same conditions and the same clock and
// they compute the same water, crest for crest, with nothing replicated. The wind is about to become
// an INPUT to that, so the moment the wind carries state -- an accumulator, a value nudged each
// frame, a draw from a running generator -- the sea inherits it and the whole property is gone. Two
// players would see different seas because their frame timings differed an hour ago.
//
// So there is no wind state. There is a function of time, evaluated wherever the answer is needed.
//
// WHAT THAT BUYS ON TOP: a wind that stops being a ramp. A value that rises to its maximum and stays
// there is what a wind looks like when it is stored rather than computed; gusts are what it looks
// like when it is a function. The envelope below spends most of its time below its own mean and
// visits the top briefly, which is the shape of real wind and the reason a lull reads as a lull.
namespace wxl::wind
{
    /// What the wind is, as a shape rather than as a value. Every field is shared: two clients given
    /// the same profile and the same time compute the same wind.
    struct WindProfile
    {
        /// Sustained speed, world units per second -- the wind with the gusts averaged out.
        float baseSpeed = 9.0f;

        /// Sustained heading in the world XY plane, degrees. The gusts veer around it; they do not
        /// replace it.
        float headingDeg = 45.0f;

        /// How far the gusts swing the speed, as a fraction of the base. At one, a lull is dead calm
        /// and a gust is twice the sustained wind. Zero is the flat wind this replaced.
        float gust = 0.55f;

        /// Seconds in the dominant gust cycle. The envelope is built from several periods around
        /// this one with no common measure, so the pattern never repeats on any timescale a player
        /// would notice -- but this is the one that sets the FEEL, and a few tens of seconds is what
        /// reads as weather rather than as a flicker.
        float gustPeriod = 17.0f;

        /// How far the heading wanders either side of the sustained one, degrees.
        ///
        /// Small on purpose. A wind that swings widely reads as broken rather than as alive, and
        /// everything downstream -- a swell that took a minute to build, grass already leaning --
        /// has inertia this does not.
        float veerDeg = 22.0f;

        /// Seconds in the dominant veer cycle. Deliberately far longer than the gust period: the
        /// direction of a wind changes on a slower schedule than its strength, and tying the two
        /// together makes every gust arrive from somewhere new, which is the giveaway.
        float veerPeriod = 53.0f;

        /// How much the client's own weather moves the sustained speed, 0..1. At zero the profile is
        /// the whole story and the wind ignores the sky; at one a storm rolling in raises the wind
        /// and the sea with it.
        float weatherCoupling = 1.0f;

        /// Fixed per-component phases come from here. Shared with every client; changing it gives a
        /// different but equally valid wind.
        uint32_t seed = 0x57494E44u;
    };

    /// The wind at one instant. Everything downstream reads this and nothing recomputes it, so the
    /// grass, the sea and the screen indicator cannot disagree about which way it is blowing.
    struct WindSample
    {
        /// Unit vector in the world XY plane.
        float dir[2] = { 1.0f, 0.0f };
        /// World units per second, gusts included.
        float speed = 0.0f;
        /// The same heading as `dir`, in degrees, for callers that want the angle rather than the
        /// vector. Carried rather than recovered with an inverse tangent at every call site.
        float headingDeg = 0.0f;
        /// Where in the gust envelope this instant sits, 0 at the bottom of a lull and 1 at the peak
        /// of a gust. What a visual indicator wants: the speed alone cannot say whether the wind is
        /// building or dying.
        float gust = 0.0f;
    };

    /// The wind at an arbitrary time. Pure -- no state is read or written, so it can be asked for
    /// the past or the future, which is what lets a caller lead or lag it without a second field.
    WindSample WindAt(const WindProfile& profile, double seconds);

    /// The live profile, as the panel edits it.
    WindProfile& Settings();

    /// This frame's wind. Resolved once at the frame boundary, for the same reason the sea's table
    /// is: two consumers evaluating a millisecond apart would disagree, and the whole point of one
    /// wind is that they cannot.
    const WindSample& Frame();

    /// Seconds the wind is measured from.
    ///
    /// The session's own start, exactly like the sea's clock, and it carries the same gap: a local
    /// origin means two players see different wind. One shared epoch fixes the wind and the sea
    /// together, and this is one of the two functions it has to reach.
    double Now();
}
