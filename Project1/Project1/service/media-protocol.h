#pragma once

#include <string>
#include <vector>

std::string encodeMediaProtocol(const std::string &json);
std::string decodeMediaProtocol(const std::string &payload);
std::string decodeMediaBase64(const std::string &input);
