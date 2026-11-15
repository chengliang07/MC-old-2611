#pragma once

#include <string>

struct PetState {
    std::string name = "小丢";
    int food_balance = 0;
    int max_food = 99;
    int satiety = 70;
    std::string mood = "egg";
    int level = 1;
    int xp = 0;
    int next_level_xp = 20;
    int xp_to_next = 20;
    std::string stage = "egg";
    bool hatched = false;
    bool dormant = false;
    int hatch_food_required = 3;
    int today_earned = 0;
    int daily_cap = 20;
    int reward_remembered = 2;
    int reward_forgotten = 1;
};
