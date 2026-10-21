// builtins.h - commands implemented inside the shell itself.
#pragma once

#include <string>
#include <vector>

bool isBuiltin(const std::string& name);
int runBuiltin(const std::vector<std::string>& argv);
