/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The priority of the UPnP copy of a host candidate (IcePriority.h).
 *
 * What a same-LAN session relies on: the copy at the public address ranks below
 * the LAN host candidate it copies, the copies of several interfaces keep their
 * order, and only the priority field of the line changes.
 */

#include "../src/streaming/IcePriority.h"

#include "test_framework.h"

#include <string>

namespace {

void testSrflxPriorityRanksBelowHost()
{
    // The two candidates of the hairpinned session, 09/10/2026: the LAN one
    // and its copy went out at the same 2114976511.
    const uint32_t host = 2114976511u;
    CHECK_EQ(host >> 24, mw::ice::kHostTypePreference);

    const uint32_t copy = mw::ice::srflxPriority(host);
    CHECK(copy < host);
    CHECK_EQ(copy >> 24, mw::ice::kSrflxTypePreference);
    CHECK_EQ(copy & 0x00FFFFFFu, host & 0x00FFFFFFu);

    // Below every host candidate, whatever its local preference.
    CHECK(copy < (mw::ice::kHostTypePreference << 24));

    // Two interfaces' copies keep the order of their host candidates.
    CHECK(mw::ice::srflxPriority(2114977791u) > mw::ice::srflxPriority(2114976511u));
}

void testWithPriorityRewritesOnlyThePriority()
{
    const std::string line = "candidate:6 1 UDP 2114976511 82.67.150.202 46102 typ host";
    CHECK_EQ(mw::ice::withPriority(line, 1678768895u),
             std::string("candidate:6 1 UDP 1678768895 82.67.150.202 46102 typ host"));

    // A tail (TCP type, generation) is kept as it is.
    const std::string tcp =
        "candidate:2 1 TCP 2105524479 82.67.150.202 46102 typ host tcptype passive";
    CHECK_EQ(mw::ice::withPriority(tcp, 7u),
             std::string("candidate:2 1 TCP 7 82.67.150.202 46102 typ host tcptype passive"));
}

void testWithPriorityLeavesAMalformedLineAlone()
{
    CHECK_EQ(mw::ice::withPriority("", 1u), std::string());
    CHECK_EQ(mw::ice::withPriority("candidate:6 1 UDP", 1u), std::string("candidate:6 1 UDP"));
    const std::string notANumber = "candidate:6 1 UDP high 82.67.150.202 46102 typ host";
    CHECK_EQ(mw::ice::withPriority(notANumber, 1u), notANumber);
}

} // namespace

void run_ice_priority_tests()
{
    testSrflxPriorityRanksBelowHost();
    testWithPriorityRewritesOnlyThePriority();
    testWithPriorityLeavesAMalformedLineAlone();
}
