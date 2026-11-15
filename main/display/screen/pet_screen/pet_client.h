#pragma once

#include <string>

#include "pet_types.h"

class PetClient {
public:
    static bool Fetch(PetState& state, std::string& error);
    static bool Feed(PetState& state, std::string& error, std::string& code);
    static bool Hatch(PetState& state, std::string& error, std::string& code);
};
