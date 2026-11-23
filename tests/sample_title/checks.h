// Shared by the sample title's check files.
#pragma once

void check(bool ok, const char* what);

// Set when the title runs with the host network allowed.
extern bool g_expect_online;

void test_game_runtime();
void test_graphics();
