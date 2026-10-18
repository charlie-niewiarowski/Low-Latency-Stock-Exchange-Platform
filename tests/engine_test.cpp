//
// Created by cniew on 9/11/26.
//

#include "engine.hpp"

#include <gtest/gtest.h>

class EngineTest : public testing::Test {
protected:
    int throwaway = 0;
};

TEST_F(EngineTest, empty_test) {
    ASSERT_TRUE(true);
}