#pragma once
#include <string>
#include <vector>
namespace SourceChoices
{
struct Choice {std::string label,value;};
std::vector<Choice> enumerate(bool camera);
}
