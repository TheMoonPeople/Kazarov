#include <gtest/gtest.h>
#include <libpad.h>

#include "platform/input.hpp"
#include "sce/libpad_extensions.hpp"
#include "sce/sdk_params.hpp"

TEST(PlatformPadLifecycle, ClosingAPortAndEndingTheLibraryReleaseOpenedPorts) {
    using Buffer = SdkParams<decltype(scePadPortOpen)>::At<2>;
    scePadInit(0);
    EXPECT_EQ(scePadPortOpen(0, 0, Buffer{}), 1);
    EXPECT_EQ(scePadPortOpen(1, 0, Buffer{}), 1);
    unsigned char motors[] = {1, 192, 0, 0, 0, 0};
    EXPECT_EQ(scePadSetActDirect(0, 0, motors), 1);
    EXPECT_EQ(scePadSetActDirect(1, 0, motors), 1);
    EXPECT_TRUE(InputGetRumble(0).small_motor);
    EXPECT_EQ(InputGetRumble(0).large_motor, 192);
    EXPECT_TRUE(InputGetRumble(1).small_motor);
    EXPECT_EQ(InputGetRumble(1).large_motor, 192);
    EXPECT_EQ(scePadPortClose(-1, 0), 0);
    EXPECT_EQ(scePadPortClose(0, 1), 0);
    EXPECT_EQ(scePadPortClose(0, 0), 1);
    EXPECT_FALSE(InputGetRumble(0).small_motor);
    EXPECT_EQ(InputGetRumble(0).large_motor, 0);
    EXPECT_TRUE(InputGetRumble(1).small_motor);
    EXPECT_EQ(InputGetRumble(1).large_motor, 192);
    EXPECT_EQ(scePadPortClose(0, 0), 0);
    EXPECT_EQ(scePadGetState(0, 0), scePadStateDiscon);
    EXPECT_EQ(scePadEnd(), 1);
    EXPECT_FALSE(InputGetRumble(1).small_motor);
    EXPECT_EQ(InputGetRumble(1).large_motor, 0);
    EXPECT_EQ(scePadPortClose(1, 0), 0);
    EXPECT_EQ(scePadEnd(), 1);
    EXPECT_EQ(scePadPortOpen(0, 0, Buffer{}), 1);
    EXPECT_EQ(scePadPortClose(0, 0), 1);
}
